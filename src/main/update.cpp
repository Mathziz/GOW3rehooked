// SPDX-License-Identifier: GPL-2.0-or-later
// Windows release updates, inside the runtime. The latest GitHub release is offered through a `gow3.exe --update-offer`
// child (the prompt the Linux AppImage updater uses too); on Yes its ZIP replaces gow3.dll and the licenses in place and the
// game restarts. The launchers are never replaced: Explorer shortcuts keep pointing at the files the player approved.
#include "main/update.h"
#include "main/host.h"
#include <windows.h>
#include <unknwn.h>
#include <bcrypt.h>
#include <shobjidl_core.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <stb_image.h> // its raw DEFLATE decoder reads the release ZIP; implemented in ui/ui_bridge.cpp
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Gow3::Update {
namespace {
constexpr std::wstring_view Repository = L"cuesta4/GOW3rehooked";
constexpr std::string_view Runtime = "gow3.dll", Retired = "gow3_update.exe",
                           Cache = "_rehooked_/shader_cache/recipes-hash2-aes.bin";
constexpr std::string_view Launchers[]{"gow3.exe", "gow3_debug.exe"};
constexpr uint64_t MaxArchive = 128ull << 20, MaxContents = 256ull << 20;
#ifdef GOW3_IVY_BRIDGE
constexpr std::wstring_view Asset = L"GOW3rehooked-Windows-x86_64-AVX1.zip";
#else
constexpr std::wstring_view Asset = L"GOW3rehooked-Windows-x86_64.zip";
#endif

[[noreturn]] void Fail(const std::string& what) { throw std::runtime_error(what); }
std::string Utf8(const fs::path& path) { return winrt::to_string(path.native()); }
bool IsLauncher(std::string_view name) { return std::ranges::find(Launchers, name) != std::end(Launchers); }

// Release order: major, minor, patch, then alpha < beta < rc < final, then the pre-release number.
using Version = std::array<int, 5>;
std::optional<Version> ParseVersion(const std::string& tag) {
    static const std::regex pattern(R"(^v(\d{1,6})\.(\d{1,6})(?:\.(\d{1,6}))?(?:-(alpha|beta|rc)(?:\.(\d{1,6}))?)?$)");
    std::smatch match;
    if (!std::regex_match(tag, match, pattern)) return {};
    const auto number = [&](int group) { return match[group].matched ? std::stoi(match[group].str()) : 0; };
    const int stage = !match[4].matched ? 3 : match[4] == "alpha" ? 0 : match[4] == "beta" ? 1 : 2;
    return Version{number(1), number(2), number(3), stage, number(5)};
}

struct Internet {
    HINTERNET handle;
    explicit Internet(HINTERNET value) : handle(value) {}
    Internet(const Internet&) = delete;
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
};

// HTTPS GET of at most `limit` bytes. Each network step may take `step` ms, the whole transfer `total` ms.
std::vector<uint8_t> Fetch(const std::wstring& url, uint64_t limit, DWORD step, DWORD total) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) Fail("Invalid update address.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const auto deadline = GetTickCount64() + total;
    // The system proxy settings apply; redirects (GitHub's asset CDN) stay on HTTPS.
    const Internet session(WinHttpOpen(L"GOW3rehooked-updater/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle || !WinHttpSetTimeouts(session.handle, int(step), int(step), int(step), int(step)))
        Fail(HostError("WinHttpOpen"));
    const Internet connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    const Internet request(connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr);
    if (!request.handle || !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
        Fail(HostError("Connect to GitHub"));
    DWORD status{}, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        Fail(std::format("GitHub answered HTTP {}.", status));
    std::vector<uint8_t> body;
    body.reserve(size_t(std::min(limit, MaxArchive)));
    for (;;) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.handle, &available)) Fail(HostError("Download"));
        if (!available) return body;
        if (available > limit - body.size()) Fail("The download is larger than expected.");
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read{};
        if (!WinHttpReadData(request.handle, body.data() + offset, available, &read)) Fail(HostError("Download"));
        body.resize(offset + read);
        if (GetTickCount64() > deadline) Fail("The download took too long.");
    }
}

std::string Sha256(std::span<const uint8_t> data) {
    BCRYPT_ALG_HANDLE algorithm{};
    std::array<uint8_t, 32> digest{};
    const bool hashed = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                        BCRYPT_SUCCESS(BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()),
                                                  digest.data(), ULONG(digest.size())));
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashed) Fail("Could not compute the checksum of the update.");
    std::string hex;
    for (const auto byte : digest) hex += std::format("{:02x}", byte);
    return hex;
}

uint32_t Crc32(std::span<const uint8_t> data) {
    static const auto table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            entries[i] = value;
        }
        return entries;
    }();
    uint32_t crc = ~0u;
    for (const auto byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct ZipFile {
    std::string name; // '/' separated
    std::vector<uint8_t> data;
};

template <class T> T Read(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) Fail("The update ZIP is damaged.");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// The files of a ZIP, stored or DEFLATE-compressed, each checked against its CRC-32. Folders are left out.
std::vector<ZipFile> Unzip(std::span<const uint8_t> zip) {
    constexpr size_t end_record = 22;
    if (zip.size() < end_record) Fail("The update ZIP is damaged.");
    size_t end = zip.size() - end_record;
    for (const size_t first = end > 0xFFFF ? end - 0xFFFF : 0; Read<uint32_t>(zip, end) != 0x06054b50; --end)
        if (end == first) Fail("The update ZIP is damaged.");
    const auto count = Read<uint16_t>(zip, end + 10);
    size_t at = Read<uint32_t>(zip, end + 16);
    std::vector<ZipFile> files;
    uint64_t total = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (Read<uint32_t>(zip, at) != 0x02014b50) Fail("The update ZIP is damaged.");
        const auto flags = Read<uint16_t>(zip, at + 8), method = Read<uint16_t>(zip, at + 10);
        const auto crc = Read<uint32_t>(zip, at + 16), packed = Read<uint32_t>(zip, at + 20), size = Read<uint32_t>(zip, at + 24);
        const auto name_size = Read<uint16_t>(zip, at + 28);
        const size_t next = at + 46 + name_size + Read<uint16_t>(zip, at + 30) + Read<uint16_t>(zip, at + 32);
        const auto local = Read<uint32_t>(zip, at + 42);
        if (at + 46 + name_size > zip.size()) Fail("The update ZIP is damaged.");
        std::string name(reinterpret_cast<const char*>(zip.data()) + at + 46, name_size);
        at = next;
        std::ranges::replace(name, '\\', '/');
        if (name.ends_with('/')) continue;
        if ((flags & 1) || (method != 0 && method != 8) || size > MaxContents - total) Fail("Unsupported file in the update ZIP: " + name);
        total += size;
        if (Read<uint32_t>(zip, local) != 0x04034b50) Fail("The update ZIP is damaged.");
        const size_t data = size_t(local) + 30 + Read<uint16_t>(zip, local + 26) + Read<uint16_t>(zip, local + 28);
        if (data > zip.size() || zip.size() - data < packed) Fail("The update ZIP is damaged.");
        ZipFile file{std::move(name), std::vector<uint8_t>(size)};
        const auto source = zip.subspan(data, packed);
        const bool unpacked = method == 0 ? packed == size && (std::ranges::copy(source, file.data.begin()), true)
                                          : !size || stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(file.data.data()), int(size),
                                                         reinterpret_cast<const char*>(source.data()), int(packed)) == int(size);
        if (!unpacked || Crc32(file.data) != crc) Fail("The update ZIP is damaged: " + file.name);
        files.push_back(std::move(file));
    }
    return files;
}

// Attachment Services applies the machine's policy for downloaded programs and records where they came from.
void MarkDownloaded(const fs::path& file, const std::wstring& url) {
    constexpr CLSID attachment_services{0x4125dd96, 0xe03a, 0x4103, {0x8f, 0x70, 0xe0, 0x59, 0x7d, 0x80, 0x3b, 0x9c}};
    winrt::com_ptr<IAttachmentExecute> attachment;
    winrt::check_hresult(CoCreateInstance(attachment_services, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(attachment.put())));
    winrt::check_hresult(attachment->SetLocalPath(file.c_str()));
    winrt::check_hresult(attachment->SetSource(url.c_str()));
    winrt::check_hresult(attachment->Save());
}

void WriteNew(const fs::path& file, std::span<const uint8_t> data) {
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) Fail(HostError("Create " + Utf8(file)));
    DWORD written{};
    const bool complete = ::WriteFile(handle, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
    const auto error = complete ? std::string() : HostError("Write " + Utf8(file));
    CloseHandle(handle);
    if (!complete) Fail(error);
}

// Destination subfolders may already belong to the player: they must be plain folders, not links.
void PrepareFolder(const fs::path& folder) {
    CreateDirectoryW(folder.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        Fail("The update destination is not a plain folder: " + Utf8(folder));
}

struct Replacement {
    fs::path source, target, backup;
    bool moved{}, installed{};
};

// Moves every file of the release into `app`, the runtime last, then calls `restart`. A failure puts the previous files back.
void Install(std::vector<ZipFile> files, const fs::path& app, const std::wstring& url, const std::function<void()>& restart) {
    std::vector<std::string> names;
    for (const auto& file : files) {
        const auto& name = file.name;
        static const std::regex license(R"(^licenses/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.txt$)");
        if (name != Runtime && !IsLauncher(name) && name != Retired && name != Cache && !std::regex_match(name, license))
            Fail("Unexpected file in the release: " + name);
        auto folded = name;
        std::ranges::transform(folded, folded.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (std::ranges::find(names, folded) != names.end() || file.data.empty()) Fail("Invalid release contents.");
        names.push_back(std::move(folded));
    }
    for (const auto required : {Runtime, Launchers[0], Launchers[1]})
        if (std::ranges::find(names, required) == names.end()) Fail("The release has no " + std::string(required) + ".");

    const auto stage = app / std::format(L".gow3-update-{}-{}", GetCurrentProcessId(), GetTickCount64());
    if (!CreateDirectoryW(stage.c_str(), nullptr)) Fail(HostError("Create " + Utf8(stage)));
    bool keep_stage = false;
    const struct RemoveStage {
        const fs::path& stage;
        const bool& keep;
        ~RemoveStage() { std::error_code error; if (!keep) fs::remove_all(stage, error); }
    } remove_stage{stage, keep_stage};
    std::vector<Replacement> replacements;
    try {
        for (const auto& file : files) {
            // gow3_update.exe stays in releases for the updaters of v0.1.5 and v0.1.6; this one leaves it out.
            if (file.name == Retired) continue;
            const fs::path relative(std::wstring(file.name.begin(), file.name.end())); // checked ASCII names
            const auto target = app / relative.lexically_normal();
            if ((file.name == Cache || IsLauncher(file.name)) && fs::exists(target)) continue;
            auto folder = app;
            for (const auto& part : relative.parent_path()) PrepareFolder(folder /= part);
            const auto source = stage / relative.filename();
            WriteNew(source, file.data);
            if (file.name == Runtime || IsLauncher(file.name)) MarkDownloaded(source, url);
            replacements.push_back({source, target, stage / (relative.filename().native() + L".old")});
        }
        std::ranges::stable_partition(replacements, [](const Replacement& r) { return r.target.filename() != L"gow3.dll"; });
        for (auto& replacement : replacements) {
            if (fs::exists(replacement.target)) {
                // This process and the offer still map the running runtime: it can be renamed, not overwritten.
                if (replacement.target.filename() == L"gow3.dll") {
                    replacement.backup = app / L"gow3.update.old";
                    DeleteFileW(replacement.backup.c_str());
                }
                if (!MoveFileExW(replacement.target.c_str(), replacement.backup.c_str(), 0))
                    Fail(HostError("Move " + Utf8(replacement.target)));
                replacement.moved = true;
            }
            if (!MoveFileExW(replacement.source.c_str(), replacement.target.c_str(), 0)) Fail(HostError("Install " + Utf8(replacement.target)));
            replacement.installed = true;
        }
        restart();
    } catch (...) {
        std::string failures;
        for (auto& replacement : replacements | std::views::reverse) {
            bool restored = !replacement.installed || DeleteFileW(replacement.target.c_str());
            restored = restored && (!replacement.moved || MoveFileExW(replacement.backup.c_str(), replacement.target.c_str(), 0));
            if (!restored) failures += "\n" + HostError("Restore " + Utf8(replacement.target));
        }
        if (!failures.empty()) {
            keep_stage = true;
            Fail("Could not restore every file. Backups: " + Utf8(stage) + failures);
        }
        throw;
    }
}

// The update prompt runs in `gow3.exe --update-offer`: it writes Y or N on its stdout and closes when its stdin does.
class Offer {
public:
    Offer(const fs::path& app, const std::string& tag) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE child_input{}, child_output{};
        if (!CreatePipe(&child_input, &input_, &inherit, 0)) Fail(HostError("CreatePipe"));
        if (!CreatePipe(&output_, &child_output, &inherit, 0)) {
            const auto error = HostError("CreatePipe");
            CloseHandle(child_input);
            CloseHandle(input_);
            Fail(error);
        }
        SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_input;
        startup.hStdOutput = child_output;
        const auto exe = app / L"gow3.exe";
        std::wstring command = L"\"" + exe.native() + L"\" --update-offer " + std::wstring(tag.begin(), tag.end());
        const bool started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process_);
        const auto error = started ? std::string() : HostError("Start the update prompt");
        CloseHandle(child_input);
        CloseHandle(child_output);
        if (!started) {
            CloseHandle(input_);
            CloseHandle(output_);
            Fail(error);
        }
        CloseHandle(process_.hThread);
    }
    Offer(const Offer&) = delete;
    ~Offer() {
        Close();
        CloseHandle(output_);
        CloseHandle(process_.hProcess);
    }
    // 'Y', 'N', or nothing when the prompt ended without an answer.
    std::optional<char> Answer() {
        char answer{};
        DWORD read{};
        if (::ReadFile(output_, &answer, 1, &read, nullptr) && read == 1) return answer;
        return {};
    }
    // Ends the prompt and waits for it to go.
    void Close() {
        if (!input_) return;
        CloseHandle(input_);
        input_ = nullptr;
        WaitForSingleObject(process_.hProcess, 10000);
    }
    DWORD ExitCode() {
        WaitForSingleObject(process_.hProcess, INFINITE);
        DWORD code{};
        GetExitCodeProcess(process_.hProcess, &code);
        return code;
    }

private:
    HANDLE input_{}, output_{}; // the prompt's stdin (written here) and stdout (read here)
    PROCESS_INFORMATION process_{};
};

void Restart(const fs::path& app, const fs::path& game) {
    const auto exe = app / L"gow3.exe";
    // A trailing dot keeps a directory's final backslash away from the closing quote.
    std::wstring command = L"gow3.exe --game-dir \"" + (fs::absolute(game) / L".").native() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        Fail(HostError("Restart the game"));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

struct Release {
    std::string tag, digest;
    uint64_t size{};
};

// The latest release, when it has a ZIP for this CPU variant with a SHA-256 digest.
std::optional<Release> LatestRelease() {
    using namespace winrt::Windows::Data::Json;
    const auto body = Fetch(std::format(L"https://api.github.com/repos/{}/releases/latest", Repository), 1 << 20, 3000, 3000);
    JsonObject root{nullptr};
    if (!JsonObject::TryParse(winrt::to_hstring(std::string_view(reinterpret_cast<const char*>(body.data()), body.size())), root))
        return {};
    const auto assets = root.GetNamedArray(L"assets", JsonArray());
    for (uint32_t i = 0; i < assets.Size(); ++i) {
        if (assets.GetAt(i).ValueType() != JsonValueType::Object) continue;
        const auto asset = assets.GetObjectAt(i);
        if (std::wstring_view(asset.GetNamedString(L"name", L"")) != Asset) continue;
        Release release{winrt::to_string(root.GetNamedString(L"tag_name", L"")), winrt::to_string(asset.GetNamedString(L"digest", L""))};
        const double size = asset.GetNamedNumber(L"size", 0);
        static const std::regex digest("^sha256:[0-9a-f]{64}$");
        if (size < 1 || size > double(MaxArchive) || !std::regex_match(release.digest, digest)) return {};
        release.size = uint64_t(size);
        return release;
    }
    return {};
}

std::string ReadText(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

bool Check(const fs::path& app, const fs::path& game, const Version& current) {
    const auto release = LatestRelease();
    if (!release) return false;
    const auto version = ParseVersion(release->tag);
    const auto skipped = game / L"_rehooked_" / L"settings" / L"skipped-release.txt";
    if (!version || *version <= current || ReadText(skipped) == release->tag) return false;
    Offer offer(app, release->tag);
    const auto answer = offer.Answer();
    if (!answer) return offer.ExitCode() == 0; // the player closed the prompt's window
    if (*answer == 'N') {
        fs::create_directories(skipped.parent_path());
        std::ofstream(skipped, std::ios::binary) << release->tag;
        return false;
    }
    if (*answer != 'Y') return true;
    std::string error;
    try {
        const auto tag = winrt::to_hstring(release->tag);
        const auto url = std::format(L"https://github.com/{}/releases/download/{}/{}", Repository, std::wstring_view(tag), Asset);
        const auto zip = Fetch(url, release->size, 30000, 180000);
        if (zip.size() != release->size || "sha256:" + Sha256(zip) != release->digest)
            Fail("The ZIP does not match the release checksum.");
        Install(Unzip(zip), app, url, [&] {
            offer.Close();
            Restart(app, game);
        });
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
    } catch (const winrt::hresult_error& failure) {
        error = winrt::to_string(failure.message());
    }
    offer.Close();
    MessageBoxW(nullptr, winrt::to_hstring(error).c_str(), L"GOW3rehooked update", MB_OK | MB_ICONERROR);
    return false;
}

// WinRT JSON and Attachment Services need COM on this thread, which goes on to run the game. Attachment Services is an
// apartment-threaded object: a single-threaded apartment uses it directly.
struct Apartment {
    bool joined{};
    Apartment() {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            joined = true;
        } catch (const winrt::hresult_error&) {
            // Already in the multithreaded apartment: COM reaches the object through its proxy.
        }
    }
    Apartment(const Apartment&) = delete;
    ~Apartment() { if (joined) winrt::uninit_apartment(); }
};
} // namespace

void RemoveRetiredUpdater(const fs::path& app) {
    for (const auto* name : {L"gow3_update.exe", L"gow3_update.update.old"}) DeleteFileW((app / name).c_str());
}

bool OfferAndInstall(const fs::path& app, const fs::path& game, std::wstring_view tag) {
    const auto current = ParseVersion(winrt::to_string(tag));
    if (!current) return false; // local builds (v0.0.0-local) never update
    // One check per installation at a time; a second launch meanwhile starts the game without one.
    std::wstring folded = app.native();
    CharUpperBuffW(folded.data(), DWORD(folded.size()));
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t c : folded) hash = (hash ^ c) * 1099511628211ull;
    const HANDLE lock = CreateMutexW(nullptr, FALSE, std::format(L"Local\\GOW3rehooked-update-{:016x}", hash).c_str());
    if (!lock) return false;
    const auto wait = WaitForSingleObject(lock, 0);
    bool handled = false;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        try {
            const Apartment apartment;
            handled = Check(app, game, *current);
        } catch (...) {
            // Discovery is optional: without GitHub the game starts as it is.
        }
        ReleaseMutex(lock);
    }
    CloseHandle(lock);
    return handled;
}
} // namespace Gow3::Update
