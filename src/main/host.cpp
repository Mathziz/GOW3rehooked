// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/host.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#endif

namespace Gow3 {
namespace {
std::mutex log_mutex;
std::array<std::string, 4096> messages;
size_t message_begin{}, message_count{}, dropped_messages{};
std::deque<std::string> debug_messages;
#ifdef _WIN32
const auto invalid_file = INVALID_HANDLE_VALUE;
#else
constexpr int invalid_file = -1;
#endif
auto debug_file = invalid_file; // debug: opened by the first FlushLog(), then Log() writes through
std::string debug_line;
}

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(512, L'\0');
    for (;;) {
        auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) throw std::runtime_error(HostError("GetModuleFileNameW"));
        if (size < path.size()) {
            path.resize(size);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

[[noreturn]] void ExitImmediately(int status) {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), status);
    ExitProcess(status);
#else
    std::_Exit(status);
#endif
}

namespace {
std::filesystem::path port_dir;
}

std::filesystem::path PreparePortDirectory(const std::filesystem::path& game) {
    const auto root = std::filesystem::absolute(game).lexically_normal();
    if (!std::filesystem::is_regular_file(root / "eboot.bin"))
        throw std::runtime_error("Cannot find eboot.bin in " + root.string());
    const auto port = root / PortDir::Root;
    std::filesystem::create_directories(port / PortDir::Settings);
    port_dir = port;
    return port;
}

const std::filesystem::path& PortDirectory() {
    static const std::filesystem::path fallback = PortDir::Root;
    return port_dir.empty() ? fallback : port_dir;
}

namespace {
enum class LaunchPhase : std::uint32_t { Closed, Starting, Ready };
constexpr std::uint32_t kSessionMagic = 0x534c3347, kSessionFormat = 1; // "G3LS"
constexpr auto kAotCooldown = std::chrono::minutes(10);
struct LaunchSession {
    std::uint32_t magic = kSessionMagic, format = kSessionFormat;
    LaunchPhase phase = LaunchPhase::Closed;
    std::uint32_t reserved = 0;
    std::int64_t aot_time = 0; // UTC seconds; preserved across skipped launches
} session;
std::mutex session_mutex;
std::filesystem::path session_path;
bool aot_building = false;

std::int64_t SessionTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Only lifecycle transitions write; replacement keeps the previous complete record if writing is interrupted.
void SaveSession() {
    if (session_path.empty()) return;
    auto temporary = session_path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&session), sizeof(session));
        out.flush();
        if (!out) return Log("Cannot write launch session");
    }
    if (!ReplaceFile(temporary, session_path))
        return Log("Cannot replace launch session");
    std::error_code error;
    std::filesystem::remove(session_path.parent_path() / "launch.flag", error); // retire the old crash marker
}
}

bool BeginLaunchSession(const std::filesystem::path& port) {
    std::scoped_lock lock(session_mutex);
    session_path = port / "launch.session";
    LaunchSession previous;
    std::ifstream in(session_path, std::ios::binary);
    const bool valid = in.read(reinterpret_cast<char*>(&previous), sizeof(previous)) && in.peek() == std::char_traits<char>::eof() &&
        previous.magic == kSessionMagic && previous.format == kSessionFormat && previous.phase <= LaunchPhase::Ready && previous.aot_time >= 0;
    in.close();
    session = valid ? previous : LaunchSession{};
    std::error_code error;
    if (session.phase == LaunchPhase::Starting || (!valid && std::filesystem::exists(port / "launch.flag", error))) return true;
    session.phase = LaunchPhase::Starting;
    SaveSession();
    return false;
}

void MarkLaunchReady() {
    std::scoped_lock lock(session_mutex);
    if (session_path.empty() || session.phase != LaunchPhase::Starting) return;
    session.phase = LaunchPhase::Ready;
    SaveSession();
}

void EndLaunch() {
    std::scoped_lock lock(session_mutex);
    if (session_path.empty()) return;
    if (aot_building) session.aot_time = SessionTime(); // orderly cancellation of a long build
    session.phase = LaunchPhase::Closed;
    SaveSession();
}

bool AotBuildAllowed() {
    std::scoped_lock lock(session_mutex);
    const auto now = SessionTime();
    return !session.aot_time || now < session.aot_time ||
        now - session.aot_time >= std::chrono::duration_cast<std::chrono::seconds>(kAotCooldown).count();
}

void BeginAotBuild() {
    std::scoped_lock lock(session_mutex);
    if (aot_building) return;
    aot_building = true;
    session.aot_time = SessionTime(); // a crash during compilation also trips the breaker
    SaveSession();
}

void EndAotBuild() {
    std::scoped_lock lock(session_mutex);
    if (!aot_building) return; // indexing alone must not delay retries or newly recorded recipes
    aot_building = false;
    if (session.phase == LaunchPhase::Closed) return;
    session.aot_time = SessionTime(); // the full cooldown starts after even a long build
    SaveSession();
}

void ResetAotCooldown() {
    std::scoped_lock lock(session_mutex);
    if (!session.aot_time) return;
    session.aot_time = 0;
    SaveSession();
}

std::vector<unsigned char> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    const auto size = file.tellg();
    if (size < 0 || size > 1024LL * 1024 * 1024)
        throw std::runtime_error("Invalid file size: " + path.string());
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}

std::string HostError(std::string_view action) {
#ifdef _WIN32
    return std::string(action) + " failed (Win32 " + std::to_string(GetLastError()) + ")";
#else
    return std::string(action) + ": " + std::generic_category().message(errno);
#endif
}

namespace {
void Append(std::string_view message) {  // under log_mutex
    if (DebugEnabled()) {
        if (debug_file == invalid_file) { debug_messages.emplace_back(message); return; }
        // Straight to the OS: the line survives a crash of the process.
        debug_line.assign(message).push_back('\n');
#ifdef _WIN32
        DWORD written;
        WriteFile(debug_file, debug_line.data(), DWORD(debug_line.size()), &written, nullptr);
#else
        std::string_view pending(debug_line);
        while (!pending.empty()) {
            const auto n = write(debug_file, pending.data(), pending.size());
            if (n > 0) pending.remove_prefix(n);
            else if (n == 0 || errno != EINTR) break;
        }
#endif
        return;
    }
    if (message_count == messages.size()) {
        message_begin = (message_begin + 1) % messages.size();
        --message_count;
        ++dropped_messages;
    }
    messages[(message_begin + message_count++) % messages.size()].assign(message);
}
} // namespace

void Log(std::string_view message) {
    std::scoped_lock lock(log_mutex);
    Append(message);
}

void TryLog(std::string_view message) {
    std::unique_lock lock(log_mutex, std::defer_lock);
    for (int i = 0; i < 50 && !lock.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (lock) Append(message);
}

void FlushLog(const std::filesystem::path& path) {
    std::scoped_lock lock(log_mutex);
    if (debug_file != invalid_file) return; // written through already
    std::ofstream file(path, std::ios::app | std::ios::binary);
    if (!file) throw std::runtime_error("Cannot write " + path.string());
    if (dropped_messages) file << "Log ring discarded " << dropped_messages << " older messages\n";
    for (const auto& message : debug_messages) file << message << '\n';
    for (size_t i = 0; i < message_count; ++i)
        file << messages[(message_begin + i) % messages.size()] << '\n';
    file.flush();
    if (!file) throw std::runtime_error("Cannot flush " + path.string());
    message_begin = message_count = dropped_messages = 0;
    debug_messages.clear();
    file.close();
#ifdef _WIN32
    if (DebugEnabled() && (debug_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) == INVALID_HANDLE_VALUE)
        throw std::runtime_error(HostError("CreateFileW " + path.string()));
#else
    if (DebugEnabled() && (debug_file = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)) == invalid_file)
        throw std::runtime_error(HostError("Open " + path.string()));
#endif
}
}
