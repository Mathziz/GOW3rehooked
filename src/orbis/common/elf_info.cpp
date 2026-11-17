// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cctype>
#include "orbis/common/elf_info.h"
#include "orbis/common/util.h"
#include "orbis/core/file_format/npbind.h"
#include "orbis/core/file_format/psf.h"
#include "main/host.h"
#include "orbis/core/linker.h"

namespace Common {
void ElfInfo::Initialize(const std::filesystem::path& game) {
    auto& psf = *Singleton<PSF>::Instance();
    if (!psf.Open(game / "sce_sys/param.sfo")) throw std::runtime_error("Invalid game param.sfo");
    game_folder = game;
    title = psf.GetString("TITLE").value_or("God of War III");
    game_serial = psf.GetString("TITLE_ID").value_or("CUSA01715");
    app_ver = psf.GetString("APP_VER").value_or("01.00");
    raw_firmware_ver = psf.GetInteger("SYSTEM_VER").value_or(0);
    firmware_ver = raw_firmware_ver & 0xFFF00000;
    Core::Loader::Elf executable;
    executable.Open(game / "eboot.bin");
    for (const auto& segment : executable.GetProgramHeader()) {
        if (segment.p_type != PT_SCE_PROCPARAM) continue;
        Core::OrbisProcParam param{};
        executable.LoadSegment(reinterpret_cast<u64>(&param), segment.p_offset,
                               std::min<u64>(segment.p_filesz, sizeof(param)));
        sdk_ver = static_cast<u32>(param.sdk_version);
    }
    psf_attributes.raw = psf.GetInteger("ATTRIBUTE").value_or(0);
    // Trophy index map (upstream ExtractTrophies, without extracting the TRP contents): without it
    // sceNpTrophyRegisterContext fails and the shell stays in its "Trophies failed to initialize" state.
    NPBindFile npbind;
    if (npbind.Load((game / "sce_sys/npbind.dat").string())) npCommIds = npbind.GetNpCommIds();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(game / "sce_sys/trophy", ec)) {
        auto name = entry.path().filename().string();
        std::ranges::transform(name, name.begin(), [](char c) { return char(std::tolower(u8(c))); });
        if (!name.starts_with("trophy") || !name.ends_with(".trp")) continue;
        if (const s32 index = std::atoi(name.c_str() + 6); index >= 0 && index < s32(npCommIds.size()))
            trophy_index_map[index] = npCommIds[index];
    }
    initialized = true;
}
}
