// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <magic_enum/magic_enum.hpp>
#include <fmt/format.h>
#include <atomic>
#include <filesystem>
#include <optional>

#include "orbis/common/elf_info.h"
#include "orbis/common/util.h"
#include "orbis/logging/log.h"
#include "orbis/internals/libs.h"
#include "orbis/internals/save_data/save_instance.h"
#include "orbis/internals/system/commondialog.h"
#include "savedatadialog.h"
#include "savedatadialog_ui.h"

#include "main/host.h"

namespace Libraries::SaveData::Dialog {

using CommonDialog::Error;
using CommonDialog::Result;
using CommonDialog::Status;

static auto g_status = Status::NONE;
static SaveDialogState g_state{};
static SaveDialogResult g_result{};
static SaveDialogUi g_save_dialog_ui;
static std::atomic<bool> continue_requested;

namespace fs = std::filesystem;

std::optional<fs::file_time_type> ContinueSaveTime(const fs::path& path) {
    std::error_code error;
    const auto data = path / "SAVEDATA";
#ifdef _WIN32
    const bool user_data = _wcsicmp(path.filename().c_str(), L"USERDATA") == 0;
#else
    const bool user_data = Common::ToLower(path.filename().string()) == "userdata";
#endif
    if (user_data ||
        !fs::is_regular_file(data, error) || !SaveInstance::IsSearchableSlot(path, error) ||
        fs::exists(path / "sce_sys" / "corrupted", error) || error) return std::nullopt;
    // Even a read-only load can rewrite param.sfo. Use the gameplay data's write time.
    const auto time = fs::last_write_time(data, error);
    if (error) return std::nullopt;
    return time;
}

bool HasContinueSave() {
    std::error_code error;
    const auto root = SaveInstance::MakeTitleSavePath(0, {});
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (ContinueSaveTime(it->path())) return true;
    }
    return false;
}

static SaveDialogResult ContinueResult(const OrbisSaveDataDialogParam& param) {
    SaveDialogResult result{.mode = param.mode, .result = Result::USER_CANCELED, .user_data = param.userData};
    if (param.mode != SaveDataDialogMode::LIST || !param.items || !param.items->dirName) return result;

    const auto& items = *param.items;
    const auto root = SaveInstance::MakeTitleSavePath(items.userId, {});
    std::optional<fs::file_time_type> newest;
    for (u32 i = 0; i < items.dirNameNum; ++i) {
        const auto dir = items.dirName[i].data.to_view();
        if (dir.empty()) continue;
        const auto path = root / dir;
        const auto time = ContinueSaveTime(path);
        if (!time || (newest && *time <= *newest)) continue;

        PSF metadata;
        std::error_code error;
        const auto sfo = path / "sce_sys" / "param.sfo";
        if (fs::file_size(sfo, error) < sizeof(PSFHeader) || error || !metadata.Open(sfo)) continue;
        newest = time;
        result.dir_name = dir;
        result.param = std::move(metadata);
        result.result = Result::OK;
    }
    return result;
}

void RequestContinue(bool requested) {
    continue_requested.store(requested);
}

Error PS4_SYSV_ABI sceSaveDataDialogClose() {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    GOW3_DIAG Gow3::Log("DIALOG_TRACE save close requested by game");
    g_save_dialog_ui.Finish(ButtonId::INVALID);
    g_save_dialog_ui.Reset();
    return Error::OK;
}

Error PS4_SYSV_ABI sceSaveDataDialogGetResult(OrbisSaveDataDialogResult* result) {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status != Status::FINISHED) {
        return Error::NOT_FINISHED;
    }
    if (result == nullptr) {
        return Error::ARG_NULL;
    }
    g_result.CopyTo(*result);
    return Error::OK;
}

Status PS4_SYSV_ABI sceSaveDataDialogGetStatus() {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    LOG_TRACE(Lib_SaveDataDialog, "called status={}", magic_enum::enum_name(g_status));
    return g_status;
}

Error PS4_SYSV_ABI sceSaveDataDialogInitialize() {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (!CommonDialog::g_isInitialized) {
        return Error::NOT_SYSTEM_INITIALIZED;
    }
    if (g_status != Status::NONE) {
        return Error::ALREADY_INITIALIZED;
    }
    if (CommonDialog::g_isUsed) {
        return Error::BUSY;
    }
    g_status = Status::INITIALIZED;
    CommonDialog::g_isUsed = true;

    return Error::OK;
}

s32 PS4_SYSV_ABI sceSaveDataDialogIsReadyToDisplay() {
    return 1;
}

Error PS4_SYSV_ABI sceSaveDataDialogOpen(const OrbisSaveDataDialogParam* param) {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status != Status::INITIALIZED && g_status != Status::FINISHED) {
        LOG_INFO(Lib_SaveDataDialog, "called without initialize");
        return Error::INVALID_STATE;
    }
    if (param == nullptr) {
        LOG_DEBUG(Lib_SaveDataDialog, "called param:(NULL)");
        return Error::ARG_NULL;
    }
    ASSERT(param->size == sizeof(OrbisSaveDataDialogParam));
    ASSERT(param->baseParam.size == sizeof(CommonDialog::BaseParam));
    const bool load_selection = param->mode == SaveDataDialogMode::LIST ||
        (param->mode == SaveDataDialogMode::SYSTEM_MSG && param->sysMsgParam && param->sysMsgParam->msgType == SystemMessageType::NODATA);
    if (param->dispType == DialogType::LOAD && load_selection && continue_requested.exchange(false)) {
        g_save_dialog_ui.Reset(true);
        g_result = ContinueResult(*param);
        g_status = Status::FINISHED;
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save continue result={} directory={}", static_cast<u32>(g_result.result), g_result.dir_name));
        return Error::OK;
    }
    if (param->mode == SaveDataDialogMode::SYSTEM_MSG && param->sysMsgParam) {
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save system_type={}", static_cast<u32>(param->sysMsgParam->msgType)));
    } else if (param->mode == SaveDataDialogMode::ERROR_CODE && param->errorCodeParam) {
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save error_code={:#x}", param->errorCodeParam->errorCode));
    }
    g_result = {};
    g_state = SaveDialogState{*param};
    g_status = Status::RUNNING;
    g_save_dialog_ui.Open(&g_state, &g_status, &g_result);
    return Error::OK;
}

Error PS4_SYSV_ABI sceSaveDataDialogProgressBarInc(OrbisSaveDataDialogProgressBarTarget target,
                                                   u32 delta) {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    if (g_state.GetMode() != SaveDataDialogMode::PROGRESS_BAR) {
        return Error::NOT_SUPPORTED;
    }
    if (target != OrbisSaveDataDialogProgressBarTarget::DEFAULT) {
        return Error::PARAM_INVALID;
    }
    g_state.GetState<SaveDialogState::ProgressBarState>().progress += delta;
    return Error::OK;
}

Error PS4_SYSV_ABI sceSaveDataDialogProgressBarSetValue(OrbisSaveDataDialogProgressBarTarget target,
                                                        u32 rate) {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    if (g_state.GetMode() != SaveDataDialogMode::PROGRESS_BAR) {
        return Error::NOT_SUPPORTED;
    }
    if (target != OrbisSaveDataDialogProgressBarTarget::DEFAULT) {
        return Error::PARAM_INVALID;
    }
    g_state.GetState<SaveDialogState::ProgressBarState>().progress = rate;
    return Error::OK;
}

Error PS4_SYSV_ABI sceSaveDataDialogTerminate() {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    if (g_status == Status::RUNNING) {
        sceSaveDataDialogClose();
    }
    if (g_status == Status::NONE) {
        return Error::NOT_INITIALIZED;
    }
    g_save_dialog_ui.Reset();
    g_status = Status::NONE;
    CommonDialog::g_isUsed = false;
    return Error::OK;
}

Status PS4_SYSV_ABI sceSaveDataDialogUpdateStatus() {
    std::scoped_lock lock{g_save_dialog_ui.GetMutex()};
    LOG_TRACE(Lib_SaveDataDialog, "called status={}", magic_enum::enum_name(g_status));
    return g_status;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("fH46Lag88XY", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogClose);
    LIB_FUNCTION("yEiJ-qqr6Cg", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogGetResult);
    LIB_FUNCTION("ERKzksauAJA", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogGetStatus);
    LIB_FUNCTION("s9e3+YpRnzw", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogInitialize);
    LIB_FUNCTION("en7gNVnh878", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogIsReadyToDisplay);
    LIB_FUNCTION("4tPhsP6FpDI", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogOpen);
    LIB_FUNCTION("V-uEeFKARJU", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogProgressBarInc);
    LIB_FUNCTION("hay1CfTmLyA", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogProgressBarSetValue);
    LIB_FUNCTION("YuH2FA7azqQ", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogTerminate);
    LIB_FUNCTION("KK3Bdg1RWK0", "libSceSaveDataDialog", 1, "libSceSaveDataDialog",
                 sceSaveDataDialogUpdateStatus);
};

} // namespace Libraries::SaveData::Dialog
