// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/save_import.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <winrt/Windows.Data.Json.h>
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
constexpr auto system_dir = L"sce_sys";
constexpr auto payload = L"SAVEDATA";
enum class Metadata { Sfo, Spartan, Missing };

fs::path FlagPath() { return PortDirectory() / PortDir::Settings / "saves.flag"; }

std::string TitleId() {
    return std::string(Common::Singleton<PSF>::Instance()->GetString("INSTALL_DIR_SAVEDATA")
                           .value_or(Common::ElfInfo::Instance().GameSerial()));
}

fs::path EnvPath(const wchar_t* key) {
    if (const auto* value = _wgetenv(key)) return value;
    return {};
}

fs::path ConfiguredPath(const fs::path& config, Source source) {
    std::error_code error;
    const auto size = fs::file_size(config, error);
    if (error || size > 1024 * 1024) return {};
    std::ifstream file(config, std::ios::binary);
    std::string json(static_cast<size_t>(size), '\0');
    if (!file.read(json.data(), json.size())) return {};
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
    return {};
}

void AddDirectory(std::vector<fs::path>& roots, const fs::path& path) {
    std::error_code error;
    if (path.empty() || !fs::is_directory(path, error)) return;
    auto canonical = fs::canonical(path, error);
    if (error) return;
    if (std::ranges::none_of(roots, [&](const auto& known) { return _wcsicmp(known.c_str(), canonical.c_str()) == 0; }))
        roots.push_back(std::move(canonical));
}

void AddShadHome(std::vector<fs::path>& titles, const fs::path& home, const fs::path& title) {
    std::error_code error;
    if (home.empty()) return;
    for (fs::directory_iterator it(home, error), end; !error && it != end; it.increment(error)) {
        const auto user = it->path().filename().native();
        if (user.find_first_not_of(L"0123456789") == user.npos)
            AddDirectory(titles, it->path() / "savedata" / title);
    }
}

std::vector<fs::path> FindTitleFolders(Source source, const fs::path& game, const fs::path& selected,
                                       const fs::path& title) {
    winrt::init_apartment();
    struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } apartment;
    std::vector<fs::path> roots, titles, legacy;
    if (!selected.empty()) {
        AddDirectory(roots, selected);
        AddDirectory(roots, selected / "user");
        if (_wcsicmp(selected.filename().c_str(), title.c_str()) == 0) AddDirectory(titles, selected);
    } else {
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
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

bool Exists(const fs::path& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool LoadMetadata(const fs::path& path) {
    if (!PlainEntry(path, false)) return false;
    const auto size = fs::file_size(path);
    PSF metadata;
    if (size > 1024 * 1024 || !metadata.Open(path)) return false;
    const auto blocks = metadata.GetBinary(SaveParams::SAVEDATA_BLOCKS);
    return !blocks || blocks->size() == sizeof(u64);
}

bool ReadSpartanParam(const fs::path& path, OrbisSaveDataParam& param) {
    static_assert(sizeof(param) == 1328 && offsetof(OrbisSaveDataParam, mtime) == 0x508);
    if (!PlainEntry(path, false) || fs::file_size(path) != sizeof(param)) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(&param), sizeof(param))) return false;
    return param.title.end() != std::find(param.title.begin(), param.title.end(), '\0') &&
           param.subTitle.end() != std::find(param.subTitle.begin(), param.subTitle.end(), '\0') &&
           param.detail.end() != std::find(param.detail.begin(), param.detail.end(), '\0');
}

fs::path SpartanMetadata(const fs::path& slot) {
    auto meta = slot;
    meta += L".sce_sys";
    return meta;
}

Metadata SlotMetadata(Source source, const fs::path& slot) {
    if (source == Source::SpartanPort && Exists(SpartanMetadata(slot) / "param.bin")) return Metadata::Spartan;
    if (source == Source::ShadPS4 || Exists(SaveInstance::GetParamSFOPath(slot))) return Metadata::Sfo;
    return Metadata::Missing;
}

std::optional<fs::file_time_type> SlotTime(Source source, const fs::path& slot) {
    const auto name = slot.filename().u8string();
    if (name.empty() || name.size() >= OrbisSaveDataDirnameDataMaxsize || name.starts_with(u8"sce_") ||
        _wcsicmp(slot.filename().c_str(), L"USERDATA") == 0 || !PlainEntry(slot, true) ||
        !PlainEntry(slot / payload, false) || !fs::file_size(slot / payload)) return std::nullopt;
    if (Exists(slot / system_dir / "corrupted")) return std::nullopt;
    switch (SlotMetadata(source, slot)) {
    case Metadata::Sfo:
        if (!PlainEntry(slot / system_dir, true) || !LoadMetadata(SaveInstance::GetParamSFOPath(slot))) return std::nullopt;
        return Dialog::ContinueSaveTime(slot);
    case Metadata::Spartan: {
        const auto meta = SpartanMetadata(slot);
        OrbisSaveDataParam param{};
        if (!PlainEntry(meta, true) || !ReadSpartanParam(meta / "param.bin", param)) return std::nullopt;
        break;
    }
    case Metadata::Missing:
        break;
    }
    return fs::last_write_time(slot / payload);
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
        if (name == L"sce_backup" || name == L"sce_backup_tmp" || name == L"sce_backup_old" ||
            (metadata != Metadata::Sfo && name == system_dir)) {
            it.disable_recursion_pending();
            continue;
        }
        if (PlainEntry(it->path(), true)) fs::create_directory(to / relative);
        else if (PlainEntry(it->path(), false)) CopyFile(it->path(), to / relative);
        else throw std::runtime_error("A save contains a link or an unreadable file.");
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
    return Dialog::ContinueSaveTime(slot) && LoadMetadata(SaveInstance::GetParamSFOPath(slot));
}
} // namespace

bool WasOffered() { return Exists(FlagPath()); }

void MarkOffered() {
    const HANDLE flag = CreateFileW(FlagPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (flag != INVALID_HANDLE_VALUE) CloseHandle(flag);
    else if (GetLastError() != ERROR_FILE_EXISTS) throw std::runtime_error(WindowsError("Record save import choice"));
}

std::optional<SaveSet> Discover(Source source, const fs::path& game, const fs::path& selected) {
    const auto destination = SaveInstance::MakeTitleSavePath(0, {});
    std::optional<SaveSet> existing;
    for (const auto& root : FindTitleFolders(source, game, selected, TitleId())) {
        SaveSet found{.root = root};
        for (const auto& entry : fs::directory_iterator(root)) {
            if (const auto time = SlotTime(source, entry.path())) {
                found.slots.push_back({entry.path().filename(), *time});
                found.conflicts += Exists(destination / entry.path().filename());
            }
        }
        if (found.slots.empty()) continue;
        std::ranges::sort(found.slots, std::greater{}, &Slot::time);
        if (found.conflicts < found.slots.size()) return found;
        if (!existing) existing = std::move(found);
    }
    return existing;
}

size_t Import(Source source, const SaveSet& saves) {
    const auto destination = SaveInstance::MakeTitleSavePath(0, {});
    const auto stage = PortDirectory() / (L".save-import-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                          std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(stage)) throw std::runtime_error("Could not prepare the save import folder.");
    std::vector<fs::path> ready;
    size_t installed = 0;
    try {
        const auto title = TitleId();
        for (const auto& slot : saves.slots) {
            if (Exists(destination / slot.name)) continue;
            const auto from = saves.root / slot.name, to = stage / slot.name;
            if (SlotTime(source, from) != slot.time) throw std::runtime_error("The source saves changed. Search again.");
            StageSlot(source, from, to, title);
            if (!ReadyToLoad(to) || Dialog::ContinueSaveTime(to) != slot.time)
                throw std::runtime_error("A copied save could not be read by the game.");
            ready.push_back(slot.name);
        }
        if (ready.empty()) throw std::runtime_error("There are no new saves to import.");
        fs::create_directories(destination);
        for (const auto& name : ready) {
            const auto target = destination / name;
            if (!MoveFileExW((stage / name).c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error(WindowsError("Install save"));
            ++installed;
            if (!ReadyToLoad(target)) throw std::runtime_error("The game could not detect an imported save.");
        }
        MarkOffered();
    } catch (...) {
        while (installed) {
            const auto& name = ready[--installed];
            if (!MoveFileExW((destination / name).c_str(), (stage / name).c_str(), MOVEFILE_WRITE_THROUGH))
                Log(WindowsError("Roll back imported save"));
        }
        std::error_code error;
        fs::remove_all(stage, error);
        throw;
    }
    std::error_code error;
    fs::remove_all(stage, error);
    return installed;
}
} // namespace Gow3::SaveImport
