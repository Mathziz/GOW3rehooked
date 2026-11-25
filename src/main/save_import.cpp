// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/save_import.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#ifdef _WIN32
#include <winrt/Windows.Data.Json.h>
#else
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "orbis/common/util.h"
#endif
#include "main/host.h"
#include "orbis/common/elf_info.h"
#include "orbis/common/path_util.h"
#include "orbis/core/file_format/psf.h"
#include "orbis/internals/save_data/dialog/savedatadialog.h"
#include "orbis/internals/save_data/save_instance.h"
#include "orbis/internals/save_data/savedata.h"

namespace Gow3::SaveImport {
namespace {
namespace fs = std::filesystem;
using namespace Libraries::SaveData;
#ifdef _WIN32
constexpr auto system_dir = L"sce_sys";
constexpr auto payload = L"SAVEDATA";
#else
constexpr auto system_dir = "sce_sys";
constexpr auto payload = "SAVEDATA";
#endif
enum class Metadata { Sfo, Spartan, Missing };

void LogPath(const fs::path& path, std::string_view reason) {
    Log(std::format("Save import: {}: {}", Common::FS::PathToUTF8String(path), reason));
}

fs::path FlagPath() { return PortDirectory() / PortDir::Settings / "saves.flag"; }

std::string TitleId() {
    return std::string(Common::Singleton<PSF>::Instance()->GetString("INSTALL_DIR_SAVEDATA")
                           .value_or(Common::ElfInfo::Instance().GameSerial()));
}

#ifdef _WIN32
fs::path EnvPath(const wchar_t* key) {
    if (const auto* value = _wgetenv(key)) return value;
    return {};
}
#else
fs::path EnvPath(const char* key) {
    if (const auto* value = std::getenv(key); value && *value) return fs::path(value);
    return {};
}

// Minimal JSON reader for launcher config files on Linux (extracts one string field at depth 1 or 2).
struct JsonReader {
    std::string_view input;
    size_t pos{};

    void SkipWhitespace() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n')) ++pos;
    }

    bool Consume(char c) {
        SkipWhitespace();
        if (pos >= input.size() || input[pos] != c) return false;
        ++pos;
        return true;
    }

    static bool ParseHex4(std::string_view s, u32& out) {
        if (s.size() < 4) return false;
        out = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = s[i];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= u32(c - '0');
            else if (c >= 'a' && c <= 'f') out |= u32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= u32(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void AppendUtf8(std::string& out, u32 cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ReadString(std::string& out) {
        out.clear();
        if (!Consume('"')) return false;
        while (pos < input.size()) {
            const unsigned char ch = static_cast<unsigned char>(input[pos++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
            if (pos >= input.size()) return false;
            switch (input[pos++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                u32 code = 0;
                if (pos + 4 > input.size() || !ParseHex4(input.substr(pos, 4), code)) return false;
                pos += 4;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    u32 low = 0;
                    if (pos + 6 > input.size() || input[pos] != '\\' || input[pos + 1] != 'u' ||
                        !ParseHex4(input.substr(pos + 2, 4), low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    pos += 6;
                    code = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return false;
                }
                AppendUtf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool SkipValue(int depth = 0) {
        if (depth > 32) return false;
        SkipWhitespace();
        if (pos >= input.size()) return false;
        const char c = input[pos];
        if (c == '"') { std::string ignored; return ReadString(ignored); }
        if (c == '{') {
            ++pos;
            if (Consume('}')) return true;
            for (;;) {
                std::string key;
                if (!ReadString(key) || !Consume(':') || !SkipValue(depth + 1)) return false;
                if (Consume(',')) continue;
                return Consume('}');
            }
        }
        if (c == '[') {
            ++pos;
            if (Consume(']')) return true;
            for (;;) {
                if (!SkipValue(depth + 1)) return false;
                if (Consume(',')) continue;
                return Consume(']');
            }
        }
        if (input.substr(pos).starts_with("true")) { pos += 4; return true; }
        if (input.substr(pos).starts_with("false")) { pos += 5; return true; }
        if (input.substr(pos).starts_with("null")) { pos += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (input[pos] == '-') ++pos;
            if (pos >= input.size() || input[pos] < '0' || input[pos] > '9') return false;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (pos < input.size() && input[pos] == '.') {
                ++pos;
                if (pos >= input.size() || input[pos] < '0' || input[pos] > '9') return false;
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            }
            if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
                ++pos;
                if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) ++pos;
                if (pos >= input.size() || input[pos] < '0' || input[pos] > '9') return false;
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            }
            return true;
        }
        return false;
    }

    std::optional<std::string> FindStringInObject(std::string_view wanted_key) {
        if (!Consume('{')) return std::nullopt;
        if (Consume('}')) return std::nullopt;
        for (;;) {
            std::string key;
            if (!ReadString(key) || !Consume(':')) return std::nullopt;
            if (key == wanted_key) {
                std::string value;
                if (ReadString(value)) return value;
                return std::nullopt;
            }
            if (!SkipValue()) return std::nullopt;
            if (Consume(',')) continue;
            return std::nullopt;
        }
    }

    std::optional<std::string> FindNestedString(std::string_view section, std::string_view wanted_key) {
        if (!Consume('{')) return std::nullopt;
        if (Consume('}')) return std::nullopt;
        for (;;) {
            std::string key;
            if (!ReadString(key) || !Consume(':')) return std::nullopt;
            if (key == section) return FindStringInObject(wanted_key);
            if (!SkipValue()) return std::nullopt;
            if (Consume(',')) continue;
            return std::nullopt;
        }
    }
};
#endif

fs::path ConfiguredPath(const fs::path& config, Source source) {
    std::error_code error;
    const auto size = fs::file_size(config, error);
    if (error || size > 1024 * 1024) return {};
    std::ifstream file(config, std::ios::binary);
    std::string json(static_cast<size_t>(size), '\0');
    if (!file.read(json.data(), json.size())) return {};
#ifdef _WIN32
    using winrt::Windows::Data::Json::JsonObject;
    JsonObject object{nullptr};
    try {
        if (!JsonObject::TryParse(winrt::to_hstring(json), object)) return {};
        if (source == Source::ShadPS4) object = object.GetNamedObject(L"General", nullptr);
        if (!object) return {};
        const auto value = object.GetNamedString(source == Source::ShadPS4 ? L"home_dir" : L"user_dir", L"");
        if (!value.empty()) return config.parent_path() / fs::path(value.c_str());
    } catch (const winrt::hresult_error&) {
        // The standard roots still work when a launcher configuration is unreadable.
    }
#else
    std::string_view view = json;
    if (view.starts_with("\xEF\xBB\xBF")) view.remove_prefix(3);
    JsonReader reader{view};
    const auto value = source == Source::ShadPS4 ? reader.FindNestedString("General", "home_dir")
                                                 : reader.FindStringInObject("user_dir");
    if (value && !value->empty()) return config.parent_path() / fs::path(*value);
#endif
    return {};
}

void AddDirectory(std::vector<fs::path>& roots, const fs::path& path) {
    std::error_code error;
    if (path.empty() || !fs::is_directory(path, error)) return;
    auto canonical = fs::canonical(path, error);
    if (error) return;
#ifdef _WIN32
    if (std::ranges::none_of(roots, [&](const auto& known) { return _wcsicmp(known.c_str(), canonical.c_str()) == 0; }))
#else
    if (std::ranges::none_of(roots, [&](const auto& known) { return known == canonical; }))
#endif
        roots.push_back(std::move(canonical));
}

void AddShadHome(std::vector<fs::path>& titles, const fs::path& home, const fs::path& title) {
    std::error_code error;
    if (home.empty()) return;
    for (fs::directory_iterator it(home, error), end; !error && it != end; it.increment(error)) {
        const auto user = it->path().filename().native();
#ifdef _WIN32
        if (user.find_first_not_of(L"0123456789") == user.npos)
#else
        if (user.find_first_not_of("0123456789") == user.npos)
#endif
            AddDirectory(titles, it->path() / "savedata" / title);
    }
}

std::vector<fs::path> FindTitleFolders(Source source, const fs::path& game, const fs::path& selected,
                                       const fs::path& title) {
#ifdef _WIN32
    winrt::init_apartment();
    struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } apartment;
#endif
    std::vector<fs::path> roots, titles, legacy;
    if (!selected.empty()) {
        AddDirectory(roots, selected);
        AddDirectory(roots, selected / "user");
        auto folder = selected.lexically_normal();
        if (folder.filename().empty()) folder = folder.parent_path();
        const auto is_title = [&](const fs::path& path) {
#ifdef _WIN32
            return _wcsicmp(path.filename().c_str(), title.c_str()) == 0;
#else
            return Common::ToLower(path.filename().string()) == Common::ToLower(title.string());
#endif
        };
        if (is_title(folder)) AddDirectory(titles, folder);
        else if (!roots.empty() && is_title(folder.parent_path())) AddDirectory(titles, folder.parent_path());
        AddDirectory(titles, folder / title);
    } else {
#ifdef _WIN32
        const auto app_data = EnvPath(L"APPDATA");
        const auto local_data = EnvPath(L"LOCALAPPDATA");
        if (source == Source::ShadPS4 && !app_data.empty()) AddDirectory(roots, app_data / "shadPS4");
        if (source == Source::SpartanPort) {
            AddDirectory(roots, EnvPath(L"SPARTAN_USER_DIR"));
            for (const auto* key : {L"SPARTAN_PORT_DIR", L"SPARTAN_DATA_DIR"}) {
                const auto dir = EnvPath(key);
                if (!dir.empty()) { AddDirectory(roots, dir); AddDirectory(roots, dir / "user"); }
            }
        }
        const char* name = source == Source::ShadPS4 ? "shadPS4" : "SpartanPort";
        const auto executable = ExecutableDirectory();
        std::error_code error;
        for (const auto& base : {game, game.parent_path(), executable, executable.parent_path(),
                                 fs::current_path(error), local_data}) {
            if (base.empty()) continue;
            const auto has = [&](const char* file) { return fs::is_regular_file(base / file, error); };
            const bool base_is_source = source == Source::ShadPS4 ?
                has("shadPS4.exe") || has("shadPS4QtLauncher.exe") :
                has("SpartanPort.exe") || has("spartanport.ini") || !ConfiguredPath(base / "settings.json", source).empty();
            if (base_is_source) {
                AddDirectory(roots, base);
                AddDirectory(roots, base / "user");
            }
            AddDirectory(roots, base / name);
            AddDirectory(roots, base / name / "user");
            if (source == Source::ShadPS4) {
                AddDirectory(roots, base / "shadPS4QtLauncher");
                AddDirectory(roots, base / "shadPS4QtLauncher" / "user");
            }
        }
#else
        const auto home = EnvPath("HOME");
        auto xdg_data = EnvPath("XDG_DATA_HOME");
        if (xdg_data.empty() && !home.empty()) xdg_data = home / ".local" / "share";
        auto xdg_config = EnvPath("XDG_CONFIG_HOME");
        if (xdg_config.empty() && !home.empty()) xdg_config = home / ".config";
        if (source == Source::ShadPS4) {
            if (!xdg_data.empty()) AddDirectory(roots, xdg_data / "shadPS4");
            if (!xdg_config.empty()) AddDirectory(roots, xdg_config / "shadPS4");
            if (!home.empty()) {
                AddDirectory(roots, home / ".var" / "app" / "net.shadps4.shadPS4" / "data" / "shadPS4");
                AddDirectory(roots, home / ".var" / "app" / "net.shadps4.shadPS4" / "config" / "shadPS4");
            }
        } else {
            AddDirectory(roots, EnvPath("SPARTAN_USER_DIR"));
            for (const auto* key : {"SPARTAN_PORT_DIR", "SPARTAN_DATA_DIR"}) {
                const auto dir = EnvPath(key);
                if (!dir.empty()) { AddDirectory(roots, dir); AddDirectory(roots, dir / "user"); }
            }
            if (!xdg_data.empty()) AddDirectory(roots, xdg_data / "SpartanPort");
        }
        const char* name = source == Source::ShadPS4 ? "shadPS4" : "SpartanPort";
        const auto executable = ExecutableDirectory();
        const auto appimage = EnvPath("APPIMAGE");
        const auto appimage_dir = appimage.empty() ? fs::path{} : appimage.parent_path();
        std::error_code error;
        for (const auto& base : {game, game.parent_path(), executable, executable.parent_path(),
                                 appimage_dir, appimage_dir.parent_path(), fs::current_path(error), xdg_data}) {
            if (base.empty()) continue;
            const auto has = [&](const char* file) { return fs::is_regular_file(base / file, error); };
            const bool base_is_source = source == Source::ShadPS4 ?
                has("shadps4") || has("shadPS4") || has("shadPS4QtLauncher") || has("shadps4.AppImage") || has("shadPS4.AppImage") :
                has("SpartanPort") || has("spartanport") || has("spartanport.ini") || !ConfiguredPath(base / "settings.json", source).empty();
            if (base_is_source) {
                AddDirectory(roots, base);
                AddDirectory(roots, base / "user");
            }
            AddDirectory(roots, base / name);
            AddDirectory(roots, base / name / "user");
            if (source == Source::ShadPS4) {
                AddDirectory(roots, base / "shadPS4QtLauncher");
                AddDirectory(roots, base / "shadPS4QtLauncher" / "user");
            }
        }
#endif
    }
    for (const auto& root : roots) {
        if (source == Source::ShadPS4) {
            AddShadHome(titles, ConfiguredPath(root / "config.json", source), title);
            AddShadHome(titles, root / "home", title);
            if (!selected.empty()) AddShadHome(titles, root, title);
            AddDirectory(legacy, root / "savedata" / "1" / title);
        } else if (const auto custom = ConfiguredPath(root / "settings.json", source); !custom.empty()) {
            AddDirectory(titles, custom / "savedata" / "1" / title);
        }
        if (source == Source::SpartanPort) AddDirectory(titles, root / "savedata" / "1" / title);
    }
    for (const auto& path : legacy) AddDirectory(titles, path);
    return titles;
}

bool PlainEntry(const fs::path& path, bool directory) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
#else
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return false;
    return directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
#endif
}

bool Exists(const fs::path& path) {
#ifdef _WIN32
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
#endif
}

const char* MetadataError(const fs::path& path) {
    if (!PlainEntry(path, false)) return "sce_sys/param.sfo is missing, unreadable or not a regular file";
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return "could not read the size of sce_sys/param.sfo";
    if (size > 1024 * 1024) return "sce_sys/param.sfo exceeds 1 MiB";
    PSF metadata;
    if (!metadata.Open(path)) return "sce_sys/param.sfo could not be parsed";
    const auto blocks = metadata.GetBinary(SaveParams::SAVEDATA_BLOCKS);
    return blocks && blocks->size() != sizeof(u64) ? "invalid SAVEDATA_BLOCKS size in param.sfo" : nullptr;
}

bool ReadSpartanParam(const fs::path& path, OrbisSaveDataParam& param) {
    static_assert(sizeof(param) == 1328 && offsetof(OrbisSaveDataParam, mtime) == 0x508);
    std::error_code error;
    if (!PlainEntry(path, false) || fs::file_size(path, error) != sizeof(param) || error) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(&param), sizeof(param))) return false;
    return param.title.end() != std::find(param.title.begin(), param.title.end(), '\0') &&
           param.subTitle.end() != std::find(param.subTitle.begin(), param.subTitle.end(), '\0') &&
           param.detail.end() != std::find(param.detail.begin(), param.detail.end(), '\0');
}

fs::path SpartanMetadata(const fs::path& slot) {
    auto meta = slot;
#ifdef _WIN32
    meta += L".sce_sys";
#else
    meta += ".sce_sys";
#endif
    return meta;
}

Metadata SlotMetadata(Source source, const fs::path& slot) {
    if (source == Source::SpartanPort && Exists(SpartanMetadata(slot) / "param.bin")) return Metadata::Spartan;
    if (source == Source::ShadPS4 || Exists(SaveInstance::GetParamSFOPath(slot))) return Metadata::Sfo;
    return Metadata::Missing;
}

std::optional<fs::file_time_type> SlotTime(Source source, const fs::path& slot) {
    const auto reject = [&](std::string_view reason) -> std::optional<fs::file_time_type> {
        LogPath(slot, reason);
        return std::nullopt;
    };
    const auto name = slot.filename().u8string();
#ifdef _WIN32
    const bool user_data = _wcsicmp(slot.filename().c_str(), L"USERDATA") == 0;
#else
    const bool user_data = Common::ToLower(slot.filename().string()) == "userdata";
#endif
    if (user_data || name.starts_with(u8"sce_"))
        return reject("preferences or system metadata; not a gameplay slot");
    if (name.empty() || name.size() >= OrbisSaveDataDirnameDataMaxsize) return reject("invalid slot name length (maximum 31 UTF-8 bytes)");
    if (!PlainEntry(slot, true)) return reject("slot is unreadable or not a regular directory");
    const auto data = slot / payload;
    if (!PlainEntry(data, false)) return reject("SAVEDATA is missing, unreadable or not a regular file");
    std::error_code error;
    const auto size = fs::file_size(data, error);
    if (error) return reject("could not read the size of SAVEDATA");
    if (!size) return reject("SAVEDATA is empty");
    if (Exists(slot / system_dir / "corrupted")) return reject("slot is marked as corrupted");
    switch (SlotMetadata(source, slot)) {
    case Metadata::Sfo:
        if (source == Source::ShadPS4 && PlainEntry(SpartanMetadata(slot) / "param.bin", false) &&
            !Exists(SaveInstance::GetParamSFOPath(slot)))
            return reject("SpartanPort metadata found; select SpartanPort as the source");
        if (!PlainEntry(slot / system_dir, true)) return reject("sce_sys is missing, unreadable or not a regular directory");
        if (const auto* reason = MetadataError(SaveInstance::GetParamSFOPath(slot))) return reject(reason);
        break;
    case Metadata::Spartan: {
        const auto meta = SpartanMetadata(slot);
        OrbisSaveDataParam param{};
        if (!PlainEntry(meta, true)) return reject("SpartanPort .sce_sys metadata directory is unreadable or a link");
        if (!ReadSpartanParam(meta / "param.bin", param)) return reject("invalid SpartanPort param.bin (expected 1328 bytes and terminated text fields)");
        break;
    }
    case Metadata::Missing:
        break;
    }
    const auto time = fs::last_write_time(data, error);
    if (error) return reject("could not read the modification time of SAVEDATA");
    return time;
}

void CopyFile(const fs::path& from, const fs::path& to) {
    const auto time = fs::last_write_time(from);
    fs::copy_file(from, to);
    if (time != fs::last_write_time(from)) throw std::runtime_error("The source save changed during import.");
    fs::last_write_time(to, time);
}

void StageSlot(Source source, const fs::path& from, const fs::path& to, const std::string& title) {
    const auto meta = SpartanMetadata(from);
    const auto metadata = SlotMetadata(source, from);
    fs::create_directory(to);
    for (fs::recursive_directory_iterator it(from), end; it != end; ++it) {
        const auto relative = it->path().lexically_relative(from);
        const auto name = relative.begin()->native();
#ifdef _WIN32
        const bool ignored_backup = name == L"sce_backup" || name == L"sce_backup_tmp" || name == L"sce_backup_old";
#else
        const bool ignored_backup = name == "sce_backup" || name == "sce_backup_tmp" || name == "sce_backup_old";
#endif
        if (ignored_backup || (metadata != Metadata::Sfo && name == system_dir)) {
            it.disable_recursion_pending();
            continue;
        }
        if (PlainEntry(it->path(), true)) fs::create_directory(to / relative);
        else if (PlainEntry(it->path(), false)) CopyFile(it->path(), to / relative);
        else {
            LogPath(it->path(), "link or unreadable entry inside the slot");
            throw std::runtime_error("A save contains a link or an unreadable file.");
        }
    }
    if (metadata == Metadata::Sfo) return;
    PSF sfo;
    SaveInstance::SetupDefaultParamSFO(sfo, Common::FS::PathToUTF8String(to.filename()), title);
    sfo.AddBinary(std::string(SaveParams::SAVEDATA_BLOCKS), u64{32768}, true);
    auto time = fs::last_write_time(to / payload);
    if (metadata == Metadata::Spartan) {
        OrbisSaveDataParam param{};
        const auto bin = meta / "param.bin";
        if (!PlainEntry(meta, true) || !ReadSpartanParam(bin, param))
            throw std::runtime_error("A save's metadata changed during import.");
        param.ToSFO(sfo);
        time = fs::last_write_time(bin);
    }
    fs::create_directory(to / system_dir);
    const auto target = SaveInstance::GetParamSFOPath(to);
    if (!sfo.Encode(target)) throw std::runtime_error("Could not write the converted save metadata.");
    fs::last_write_time(target, time);
    if (PlainEntry(meta, true) && PlainEntry(meta / "icon0.png", false))
        CopyFile(meta / "icon0.png", to / system_dir / "icon0.png");
}

bool ReadyToLoad(const fs::path& slot) {
    return Dialog::ContinueSaveTime(slot) && !MetadataError(SaveInstance::GetParamSFOPath(slot));
}

bool SameAutosave(const fs::path& from, const fs::path& to) {
    const auto target = to / payload;
    if (!PlainEntry(target, false)) return false;
    std::ifstream left(from / payload, std::ios::binary), right(target, std::ios::binary);
    std::array<char, 16 * 1024> src, dst;
    do {
        left.read(src.data(), src.size());
        right.read(dst.data(), dst.size());
        if (left.gcount() != right.gcount() || !std::equal(src.begin(), src.begin() + left.gcount(), dst.begin()))
            return false;
    } while (left && right);
    return left.eof() && right.eof() && !left.bad() && !right.bad();
}

const char* ConflictReason(const fs::path& from, const fs::path& to) {
    if (!Exists(to)) return nullptr;
    if (from.filename() != "AUTOSAVE") return "a destination slot with this name already exists";
    if (!PlainEntry(to, true)) return "destination AUTOSAVE is not a regular directory";
    return SameAutosave(from, to) ? "the same AUTOSAVE gameplay data is already present" : nullptr;
}

fs::path AutosaveArchive(const fs::path& destination, const fs::path& stage) {
    fs::path name = "AUTOSAVE - OLD";
    for (unsigned n = 2; Exists(destination / name) || Exists(stage / name); ++n)
        name = std::format("AUTOSAVE - OLD ({})", n);
    return destination / name;
}

bool MoveNoReplace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH);
#else
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return true;
    if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) {
        if (Exists(to)) { errno = EEXIST; return false; }
        return std::rename(from.c_str(), to.c_str()) == 0;
    }
    return false;
#endif
}
} // namespace

bool WasOffered() { return Exists(FlagPath()); }

void MarkOffered() {
#ifdef _WIN32
    const HANDLE flag = CreateFileW(FlagPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (flag != INVALID_HANDLE_VALUE) CloseHandle(flag);
    else if (GetLastError() != ERROR_FILE_EXISTS) throw std::runtime_error(HostError("Record save import choice"));
#else
    const int flag = open(FlagPath().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (flag >= 0) close(flag);
    else if (errno != EEXIST) throw std::runtime_error(HostError("Record save import choice"));
#endif
}

std::optional<SaveSet> Discover(Source source, const fs::path& game, const fs::path& selected) {
    const auto destination = SaveInstance::MakeTitleSavePath(0, {});
    const auto title = TitleId();
    auto roots = FindTitleFolders(source, game, selected, title);
    // Both regional releases use the same gameplay save layout. Prefer the installed release's folder.
    for (const auto* regional_title : {"CUSA01623", "CUSA01715"}) {
        if (title == regional_title) continue;
        for (const auto& root : FindTitleFolders(source, game, selected, regional_title))
            AddDirectory(roots, root);
    }
    if (roots.empty()) {
        const auto reason = std::format("no {} save folder found (installed save ID: {}; regional folders checked: CUSA01623, CUSA01715)",
                                        source == Source::ShadPS4 ? "shadPS4" : "SpartanPort", title);
        if (selected.empty()) Log("Save import: " + reason);
        else LogPath(selected, reason);
    }
    std::optional<SaveSet> existing;
    for (const auto& root : roots) {
        SaveSet found{.root = root};
        std::error_code error;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            const auto& path = it->path();
            if (source == Source::SpartanPort && path.filename().u8string().ends_with(u8".sce_sys") &&
                Exists(path / "param.bin") && !Exists(path / payload)) continue;
            if (const auto time = SlotTime(source, path)) {
                const auto name = path.filename();
                const auto target = destination / name;
                found.slots.push_back({name, *time});
                if (const auto* reason = ConflictReason(path, target)) {
                    ++found.conflicts;
                    LogPath(path, reason);
                } else if (name == "AUTOSAVE" && Exists(target)) {
                    found.archive_autosave = true;
                }
            }
        }
        if (error) {
            LogPath(root, "could not list the save folder: " + error.message());
            continue;
        }
        if (found.slots.empty()) {
            LogPath(root, "no compatible gameplay slots");
            continue;
        }
        std::ranges::sort(found.slots, std::greater{}, &Slot::time);
        if (found.conflicts < found.slots.size()) return found;
        if (!existing) existing = std::move(found);
    }
    return existing;
}

size_t Import(Source source, const SaveSet& saves) {
    static std::mutex import_mutex;
    const std::scoped_lock lock(import_mutex);
    const auto destination = SaveInstance::MakeTitleSavePath(0, {});
#ifdef _WIN32
    const auto stage = PortDirectory() / (L".save-import-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                          std::to_wstring(GetTickCount64()));
#else
    const auto stage = PortDirectory() / (".save-import-" + std::to_string(getpid()) + "-" +
                                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#endif
    if (!fs::create_directory(stage)) throw std::runtime_error("Could not prepare the save import folder.");
    std::vector<fs::path> ready;
    fs::path archived_autosave;
    size_t installed = 0;
    try {
        const auto title = TitleId();
        for (const auto& slot : saves.slots) {
            const auto from = saves.root / slot.name, to = stage / slot.name;
            if (const auto* reason = ConflictReason(from, destination / slot.name)) {
                LogPath(from, reason);
                continue;
            }
            if (SlotTime(source, from) != slot.time) {
                LogPath(from, "slot changed since discovery");
                throw std::runtime_error("The source saves changed. Search again.");
            }
            StageSlot(source, from, to, title);
            if (!ReadyToLoad(to) || Dialog::ContinueSaveTime(to) != slot.time) {
                LogPath(from, "staged data or metadata failed validation");
                throw std::runtime_error("A copied save could not be read by the game.");
            }
            ready.push_back(slot.name);
        }
        if (ready.empty()) throw std::runtime_error("There are no new saves to import.");
        fs::create_directories(destination);
        for (const auto& name : ready) {
            const auto target = destination / name;
            if (name == "AUTOSAVE" && PlainEntry(target, true)) {
                auto archive = AutosaveArchive(destination, stage);
                if (!MoveNoReplace(target, archive)) throw std::runtime_error(HostError("Archive current autosave"));
                archived_autosave = std::move(archive);
                LogPath(archived_autosave, "preserved previous AUTOSAVE");
            }
            if (!MoveNoReplace(stage / name, target))
                throw std::runtime_error(HostError("Install save"));
            ++installed;
            if (!ReadyToLoad(target)) throw std::runtime_error("The game could not detect an imported save.");
        }
        MarkOffered();
    } catch (...) {
        while (installed) {
            const auto& name = ready[--installed];
            if (!MoveNoReplace(destination / name, stage / name))
                Log(HostError("Roll back imported save"));
        }
        if (!archived_autosave.empty() && !MoveNoReplace(archived_autosave, destination / "AUTOSAVE"))
            LogPath(archived_autosave, HostError("Restore original AUTOSAVE"));
        std::error_code error;
        fs::remove_all(stage, error);
        throw;
    }
    std::error_code error;
    fs::remove_all(stage, error);
    return installed;
}
} // namespace Gow3::SaveImport
