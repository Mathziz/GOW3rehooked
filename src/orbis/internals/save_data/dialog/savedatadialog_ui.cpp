// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <imgui.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "orbis/common/elf_info.h"
#include "orbis/common/io_file.h"
#include "orbis/common/util.h"
#include "orbis/core/file_format/psf.h"
#include "orbis/core/file_sys/fs.h"
#include "orbis/internals/save_data/save_instance.h"
#include "ui/gow3_ui_theme.h"
#include "ui/imgui_std.h"
#include "savedatadialog_ui.h"

using namespace ImGui;
using namespace Libraries::CommonDialog;
using Common::ElfInfo;

constexpr u32 OrbisSaveDataBlockSize = 32768; // 32 KiB

static std::string SpaceSizeToString(size_t size) {
    if (size >= 1024ull * 1024 * 1024) return fmt::format("{:.2f} GB", double(size) / (1024.0 * 1024 * 1024));
    if (size >= 1024 * 1024) return fmt::format("{:.2f} MB", double(size) / (1024.0 * 1024));
    if (size >= 1024) return fmt::format("{:.2f} KB", double(size) / 1024.0);
    return fmt::format("{} B", size);
}

// Everything on screen is laid out for 1080p and scaled from the display: the same measure as the cqw of the screens.
constexpr float SaveCardHeight = 82.0f, SaveRowHeight = SaveCardHeight + 12.0f;

static float UiScale() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    return std::max(0.6f, std::min(display.x / 100.0f, display.y / 56.25f) / 19.2f);
}

using Gow3::UI::Fade;

#include "main/host.h"
#include <shellapi.h>
#undef DELETE // winnt.h / wingdi.h macros that clash with DialogType::DELETE and UserMessageType::ERROR
#undef ERROR

// A plus of the given half-length: the empty slot's picture.
static void DrawPlus(ImDrawList* draw, ImVec2 center, float half, ImU32 color) {
    const float width = std::max(2.0f, half * 0.28f);
    draw->AddLine({center.x - half, center.y}, {center.x + half, center.y}, color, width);
    draw->AddLine({center.x, center.y - half}, {center.x, center.y + half}, color, width);
}

namespace Libraries::SaveData::Dialog {

static SaveParsedInfo ParseSaveItem(const SaveDialogState::Item& item) {
    SaveParsedInfo info;
    info.size_str = item.size;
    info.date_str = item.date;

    // Title resolution
    if (!item.subtitle.empty()) {
        info.display_title = item.subtitle;
    } else if (!item.title.empty() &&
               item.title.find("God of War") == std::string::npos) {
        info.display_title = item.title;
    } else if (!item.dir_name.empty()) {
        info.display_title = item.dir_name;
    } else {
        info.display_title = "Save Game";
    }

    // Auto save check
    std::string lower_title = info.display_title;
    for (char& c : lower_title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower_title.find("auto save") != std::string::npos ||
        lower_title.find("autosave") != std::string::npos ||
        item.dir_name.find("AUTOSAVE") != std::string::npos) {
        info.is_auto_save = true;
    }

    // Clean title for preview slab
    info.clean_title = info.display_title;
    auto auto_pos = info.clean_title.find("(Auto Save)");
    if (auto_pos != std::string::npos) {
        info.clean_title.erase(auto_pos, 11);
        while (!info.clean_title.empty() && (info.clean_title.front() == ' ' || info.clean_title.front() == '-')) {
            info.clean_title.erase(0, 1);
        }
    }
    if (info.clean_title.empty()) {
        info.clean_title = info.display_title;
    }

    // Parse details (Difficulty and Play time / relog)
    // Format in GOW3 param.sfo: "Difficulty: God (Normal)\nPlay time: 2:02:30"
    std::string_view details_view = item.details;
    size_t start = 0;
    while (start < details_view.size()) {
        size_t end = details_view.find('\n', start);
        if (end == std::string_view::npos) end = details_view.size();
        std::string_view line = details_view.substr(start, end - start);
        start = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);

        auto diff_pos = line.find("Difficulty:");
        if (diff_pos != std::string_view::npos) {
            std::string_view val = line.substr(diff_pos + 11);
            while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
            info.difficulty = std::string(val);
        }

        auto play_pos = line.find("Play time:");
        if (play_pos != std::string_view::npos) {
            std::string_view val = line.substr(play_pos + 10);
            while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
            std::string time_str(val);
            if (time_str.size() >= 7 && time_str[1] == ':') {
                time_str.insert(0, 1, '0');
            }
            info.play_time = std::move(time_str);
        }
    }

    return info;
}

void SaveDialogResult::CopyTo(OrbisSaveDataDialogResult& result) const {
    result.mode = this->mode;
    result.result = this->result;
    result.buttonId = this->button_id;
    if (result.dirName != nullptr) {
        result.dirName->data.FromString(this->dir_name);
    }
    if (result.param != nullptr && this->param.GetString(SaveParams::MAINTITLE).has_value()) {
        result.param->FromSFO(this->param);
    }
    result.userData = this->user_data;
}

SaveDialogState::SaveDialogState(const OrbisSaveDataDialogParam& param) {
    this->mode = param.mode;
    this->type = param.dispType;
    this->user_data = param.userData;
    if (param.optionParam != nullptr) {
        this->enable_back = {param.optionParam->back == OptionBack::ENABLE};
    }

    const auto& game_serial = Common::Singleton<PSF>::Instance()
                                  ->GetString("INSTALL_DIR_SAVEDATA")
                                  .value_or(ElfInfo::Instance().GameSerial());

    const auto item = param.items;
    this->user_id = item->userId;

    if (item->titleId == nullptr) {
        this->title_id = game_serial;
    } else {
        this->title_id = item->titleId->data.to_string();
    }

    if (item->dirName != nullptr) {
        for (u32 i = 0; i < item->dirNameNum; i++) {
            const auto dir_name = item->dirName[i].data.to_view();

            if (!dir_name.empty()) {
                LoadItem(dir_name);
            }
        }
    }

    if (type == DialogType::SAVE && item->newItem != nullptr) {
        // The slot is empty: a plus where the picture would be, whatever the game passes.
        this->new_item = Item{
            .dir_name = "",
            .title = "Create a New Save",
            .is_new = true,
        };
        this->new_item->info = ParseSaveItem(*this->new_item);
    }

    if (item->focusPos != FocusPos::DIRNAME) {
        this->focus_pos = item->focusPos;
    } else if (item->focusPosDirName != nullptr) {
        this->focus_pos = item->focusPosDirName->data.to_string();
    }
    this->style = item->itemStyle;

    switch (mode) {
    case SaveDataDialogMode::USER_MSG: {
        this->state = UserState{param};
    } break;
    case SaveDataDialogMode::SYSTEM_MSG:
        this->state = SystemState{*this, param};
        break;
    case SaveDataDialogMode::ERROR_CODE: {
        this->state = ErrorCodeState{param};
    } break;
    case SaveDataDialogMode::PROGRESS_BAR: {
        this->state = ProgressBarState{*this, param};
    } break;
    default:
        break;
    }
}

// Reads one save of the dialog's user and title into the list; false when it has no param.sfo.
bool SaveDialogState::LoadItem(std::string_view dir_name) {
    auto dir_path = SaveInstance::MakeDirSavePath(user_id, title_id, dir_name);

    auto param_sfo_path = dir_path / "sce_sys" / "param.sfo";
    if (!std::filesystem::exists(param_sfo_path)) {
        return false;
    }

    PSF param_sfo;
    param_sfo.Open(param_sfo_path);

    auto last_write = std::chrono::system_clock::to_time_t(param_sfo.GetLastWrite());
    std::string date_str = fmt::format("{:%d %b %Y · %I:%M %p}", *std::localtime(&last_write));

    size_t size = Common::FS::GetDirectorySize(dir_path);
    std::string size_str = SpaceSizeToString(size);

    auto icon_path = dir_path / "sce_sys" / "icon0.png";
    RefCountedTexture icon;
    if (std::filesystem::exists(icon_path)) {
        if (mode != SaveDataDialogMode::LIST) {
            Common::FS::IOFile icon_file(icon_path, Common::FS::FileAccessMode::Read);
            if (icon_file.IsOpen()) {
                std::vector<u8> icon_bytes(icon_file.GetSize());
                icon_file.Read(icon_bytes);
                icon = RefCountedTexture::DecodePngTexture(std::move(icon_bytes));
            }
        } else {
            icon = RefCountedTexture::DecodePngFile(icon_path);
        }
    }

    bool is_corrupted = std::filesystem::exists(dir_path / "sce_sys" / "corrupted");

    this->save_list.emplace_back(Item{
        .dir_name = std::string{dir_name},
        .icon = icon,

        .title = std::string{param_sfo.GetString(SaveParams::MAINTITLE).value_or("Unknown")},
        .subtitle = std::string{param_sfo.GetString(SaveParams::SUBTITLE).value_or("")},
        .details = std::string{param_sfo.GetString(SaveParams::DETAIL).value_or("")},
        .date = date_str,
        .size = size_str,
        .last_write = param_sfo.GetLastWrite(),
        .pfo = param_sfo,
        .is_corrupted = is_corrupted,
    });
    save_list.back().info = ParseSaveItem(save_list.back());
    return true;
}

SaveDialogState::UserState::UserState(const OrbisSaveDataDialogParam& param) {
    auto& user = *param.userMsgParam;
    this->type = user.buttonType;
    this->msg_type = user.msgType;
    this->msg = user.msg != nullptr ? std::string{user.msg} : std::string{};
}

SaveDialogState::SystemState::SystemState(const SaveDialogState& state,
                                          const OrbisSaveDataDialogParam& param) {
#define M(save, load, del)                                                                         \
    if (type == DialogType::SAVE)                                                                  \
        this->msg = save;                                                                          \
    else if (type == DialogType::LOAD)                                                             \
        this->msg = load;                                                                          \
    else if (type == DialogType::DELETE)                                                           \
        this->msg = del;                                                                           \
    else                                                                                           \
        UNREACHABLE()

    auto type = param.dispType;
    auto& sys = *param.sysMsgParam;
    this->type = sys.msgType;
    switch (sys.msgType) {
    case SystemMessageType::NODATA: {
        return_cancel = true;
        this->msg = "There is no saved data.";
    } break;
    case SystemMessageType::CONFIRM:
        show_no = true;
        M("Do you want to save?", "Do you want to load this saved data?",
          "Do you want to delete this saved data?");
        break;
    case SystemMessageType::OVERWRITE:
        show_no = true;
        M("Do you want to overwrite the existing saved data?", "##UNKNOWN##", "##UNKNOWN##");
        break;
    case SystemMessageType::NOSPACE:
        return_cancel = true;
        M(fmt::format(
              "There is not enough space to save the data. To continue {} free space is required.",
              SpaceSizeToString(sys.value * OrbisSaveDataBlockSize)),
          "##UNKNOWN##", "##UNKNOWN##");
        break;
    case SystemMessageType::PROGRESS:
        hide_ok = true;
        show_cancel = state.enable_back.value_or(false);
        M("Saving...", "Loading...", "Deleting...");
        break;
    case SystemMessageType::FILE_CORRUPTED:
        return_cancel = true;
        this->msg = "The saved data is corrupted.";
        break;
    case SystemMessageType::FINISHED:
        return_cancel = true;
        M("Saved successfully.", "Loading complete.", "Deletion complete.");
        break;
    case SystemMessageType::NOSPACE_CONTINUABLE:
        return_cancel = true;
        M(fmt::format("There is not enough space to save the data. {} free space is required.",
                      SpaceSizeToString(sys.value * OrbisSaveDataBlockSize)),
          "##UNKNOWN##", "##UNKNOWN##");
        break;
    case SystemMessageType::CORRUPTED_AND_DELETED: {
        show_cancel = state.enable_back.value_or(true);
        const char* msg1 = "The saved data is corrupted and will be deleted.";
        M(msg1, msg1, "##UNKNOWN##");
    } break;
    case SystemMessageType::CORRUPTED_AND_CREATED: {
        show_cancel = state.enable_back.value_or(true);
        const char* msg2 = "The saved data is corrupted. This saved data will be deleted and a new "
                           "one will be created.";
        M(msg2, msg2, "##UNKNOWN##");
    } break;
    case SystemMessageType::CORRUPTED_AND_RESTORE: {
        show_cancel = state.enable_back.value_or(true);
        const char* msg3 =
            "The saved data is corrupted. The data that was backed up by the system will be "
            "restored.";
        M(msg3, msg3, "##UNKNOWN##");
    } break;
    case SystemMessageType::TOTAL_SIZE_EXCEEDED:
        M("Cannot create more saved data.", "##UNKNOWN##", "##UNKNOWN##");
        break;
    default:
        msg = fmt::format("Unknown message type: {}", magic_enum::enum_name(sys.msgType));
        break;
    }
    // The game asked for a message that does not exist for this kind of dialog.
    if (msg == "##UNKNOWN##") {
        msg = fmt::format("The saved data could not be {}.", type == DialogType::SAVE ? "saved" : type == DialogType::LOAD ? "loaded" : "deleted");
    }

#undef M
}

SaveDialogState::ErrorCodeState::ErrorCodeState(const OrbisSaveDataDialogParam& param) {
    auto& err = *param.errorCodeParam;
    constexpr auto NOT_FOUND = 0x809F0008;
    constexpr auto BROKEN = 0x809F000F;
    switch (err.errorCode) {
    case NOT_FOUND:
        this->error_msg = "There is no saved data.";
        break;
    case BROKEN:
        this->error_msg = "The data is corrupted.";
        break;
    default:
        this->error_msg = fmt::format("An error has occurred. ({:X})", err.errorCode);
        break;
    }
}

SaveDialogState::ProgressBarState::ProgressBarState(const SaveDialogState& state,
                                                    const OrbisSaveDataDialogParam& param) {
    this->progress = 0;
    auto& bar = *param.progressBarParam;
    if (bar.msg != nullptr) {
        this->msg = std::string{bar.msg};
    } else {
        switch (bar.sysMsgType) {
        case ProgressSystemMessageType::INVALID:
            this->msg = "";
            break;
        case ProgressSystemMessageType::PROGRESS:
            switch (state.type) {
            case DialogType::SAVE:
                this->msg = "Saving...";
                break;
            case DialogType::LOAD:
                this->msg = "Loading...";
                break;
            case DialogType::DELETE:
                this->msg = "Deleting...";
                break;
            }
            break;
        case ProgressSystemMessageType::RESTORE:
            this->msg = "Restoring saved data...";
            break;
        }
    }
}

SaveDialogUi::SaveDialogUi(SaveDialogState* state, Status* status, SaveDialogResult* result)
    : state(state), status(status), result(result) {
    if (status && *status == Status::RUNNING) {
        first_render = true;
        focused_idx = 0;
        AddLayer(this);
    }
}

SaveDialogUi::~SaveDialogUi() {
    Finish(ButtonId::INVALID);
}

SaveDialogUi::SaveDialogUi(SaveDialogUi&& other) noexcept : Layer(other) {
    std::scoped_lock lock(draw_mutex, other.draw_mutex);
    state = std::exchange(other.state, nullptr);
    status = std::exchange(other.status, nullptr);
    result = std::exchange(other.result, nullptr);
    focused_idx = other.focused_idx;
    last_load_list = std::move(other.last_load_list);
    last_load_focus = other.last_load_focus;
    RemoveLayer(&other);
    if (status && *status == Status::RUNNING) {
        first_render = true;
        AddLayer(this);
    }
}

SaveDialogUi& SaveDialogUi::operator=(SaveDialogUi&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(draw_mutex, other.draw_mutex);
    RemoveLayer(this);
    RemoveLayer(&other);
    state = std::exchange(other.state, nullptr);
    status = std::exchange(other.status, nullptr);
    result = std::exchange(other.result, nullptr);
    focused_idx = other.focused_idx;
    last_load_list = std::move(other.last_load_list);
    last_load_focus = other.last_load_focus;
    if (status && *status == Status::RUNNING) {
        first_render = true;
        AddLayer(this);
    }
    return *this;
}

void SaveDialogUi::Open(SaveDialogState* _state, Status* _status, SaveDialogResult* _result) {
    std::unique_lock lock(draw_mutex);
    // While a chosen save is being written, the game's own overwrite question and progress box are absorbed into the
    // lingering screen (it is already on the layers); any other dialog ends it.
    const bool running = _status && *_status == Status::RUNNING;
    const bool absorbs = linger_active && running &&
        (_state->mode == SaveDataDialogMode::PROGRESS_BAR ||
         (overwrite_confirmed && _state->mode == SaveDataDialogMode::SYSTEM_MSG && _state->type == DialogType::SAVE &&
          _state->GetState<SaveDialogState::SystemState>().type == SystemMessageType::OVERWRITE));
    if (linger_active && !absorbs) EndLinger();
    if (!absorbs) RemoveLayer(this);
    this->state = _state;
    this->status = _status;
    this->result = _result;
    this->focused_idx = 0;
    prompt = Prompt::None;
    activated = -1;
    repin_focus = false;
    if (status && *status == Status::RUNNING) {
        if (state->mode == SaveDataDialogMode::LIST) last_load_list.reset();
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save open mode={} type={} items={} new_item={}",
                              static_cast<u32>(state->mode), static_cast<u32>(state->type),
                              state->save_list.size(), state->new_item.has_value()));
        switch (state->mode) {
        case SaveDataDialogMode::USER_MSG: {
            const auto& s = state->GetState<SaveDialogState::UserState>();
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save user buttons={} message_type={} text={}",
                                  static_cast<u32>(s.type), static_cast<u32>(s.msg_type), s.msg));
            break;
        }
        case SaveDataDialogMode::SYSTEM_MSG: {
            const auto& s = state->GetState<SaveDialogState::SystemState>();
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save system text={} hide_ok={} show_no={} show_cancel={}",
                                  s.msg, s.hide_ok, s.show_no, s.show_cancel));
            break;
        }
        case SaveDataDialogMode::ERROR_CODE:
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save error text={}", state->GetState<SaveDialogState::ErrorCodeState>().error_msg));
            break;
        case SaveDataDialogMode::PROGRESS_BAR:
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save progress text={}", state->GetState<SaveDialogState::ProgressBarState>().msg));
            if (state->type == DialogType::LOAD) GOW3_DIAG Gow3::Log("DIALOG_TRACE save load progress hidden");
            break;
        default: break;
        }
        if (state->mode == SaveDataDialogMode::LIST) overwrite_confirmed = false;
        // The list already asked whether to overwrite: the game's own question right after it is answered Yes unseen.
        if (overwrite_confirmed && state->mode == SaveDataDialogMode::SYSTEM_MSG && state->type == DialogType::SAVE &&
            state->GetState<SaveDialogState::SystemState>().type == SystemMessageType::OVERWRITE) {
            overwrite_confirmed = false;
            GOW3_DIAG Gow3::Log("DIALOG_TRACE save overwrite already confirmed in the list");
            Finish(ButtonId::YES);
            return;
        }
        if (absorbs) { // the progress box: the screen stays until the game closes it
            linger_deadline = std::chrono::steady_clock::time_point::max();
            return;
        }
        first_render = true;
        AddLayer(this);
    }
}

void SaveDialogUi::StartLinger(const char* title, std::string message) {
    linger = *state;
    linger_focus = focused_idx;
    linger_title = title;
    linger_message = std::move(message);
    linger_active = true;
    linger_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4); // if the game opens nothing more
}

void SaveDialogUi::EndLinger() {
    linger_active = false;
    linger.reset();
}

void SaveDialogUi::Reset(bool clear_load_list) {
    std::unique_lock lock(draw_mutex);
    EndLinger();
    if (clear_load_list) last_load_list.reset();
    if (state) {
        *state = SaveDialogState{};
    }
    state = nullptr;
    status = nullptr;
    result = nullptr;
    focused_idx = 0;
    prompt = Prompt::None;
    activated = -1;
    RemoveLayer(this);
}

void SaveDialogUi::Finish(ButtonId buttonId, Result r) {
    std::unique_lock lock(draw_mutex);
    if (linger_active && state && state->mode == SaveDataDialogMode::PROGRESS_BAR) EndLinger(); // the write is done
    if (result && state) {
        if (status && *status == Status::RUNNING && state->mode == SaveDataDialogMode::LIST &&
            state->type == DialogType::LOAD && r == Result::OK && !result->dir_name.empty()) {
            last_load_list = *state;
            last_load_focus = focused_idx;
        }
        result->mode = this->state->mode;
        result->result = r;
        result->button_id = buttonId;
        result->user_data = this->state->user_data;
        if (state->mode != SaveDataDialogMode::LIST && !state->save_list.empty()) {
            result->dir_name = state->save_list.front().dir_name;
        }
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save finish mode={} result={} button={} directory={}",
                              static_cast<u32>(state->mode), static_cast<u32>(r),
                              static_cast<u32>(buttonId), result->dir_name));
    }
    if (status) {
        *status = Status::FINISHED;
    }
    if (!linger_active) RemoveLayer(this);
}

void SaveDialogUi::DrawItem(int id, const SaveDialogState::Item& item, bool clickable, bool pin_focus) {
    auto& ctx = *GetCurrentContext();
    auto& window = *ctx.CurrentWindow;
    const ImGuiID imgui_id = GetID(id);

    const float S = UiScale();
    const float avail_w = GetContentRegionAvail().x - 16.0f * S;  // the rows stop short of the scrollbar, even grown by the focus
    const float base_card_h = SaveCardHeight * S;
    const float row_h = SaveRowHeight * S;

    const ImVec2 outer_pos = window.DC.CursorPos;
    const ImRect row_bb{outer_pos, outer_pos + ImVec2{avail_w, row_h}};

    ItemSize(ImVec2{avail_w, row_h});
    if (!ItemAdd(row_bb, imgui_id)) {
        return;
    }

    if (pin_focus) {
        if (focused_idx == id) SetItemCurrentNavFocus();
    } else if (clickable) {
        bool hovered = false, held = false;
        const bool pressed = ButtonBehavior(row_bb, imgui_id, &hovered, &held);
        const bool nav_here = ctx.NavId == imgui_id;
        // The pointer takes the focus only when it moves, and the gamepad's focus follows it, so neither fights the other.
        if (hovered && (ctx.IO.MouseDelta.x != 0.0f || ctx.IO.MouseDelta.y != 0.0f)) {
            if (!nav_here) SetFocusID(imgui_id, &window);
            focused_idx = id;
        } else if (nav_here) {
            focused_idx = id;
        }
        // A click or Cross on the save in focus selects it; a click on another only moves the focus to it.
        if (pressed) {
            if (nav_here || focused_idx == id) activated = id; // DrawList decides what the selection does
            focused_idx = id;
        }
    }

    // The focus grows the card: t goes 0 -> 1 in a few frames and every measure below interpolates with it.
    const float target = focused_idx == id ? 1.0f : 0.0f;
    float& focus = *GetStateStorage()->GetFloatRef(imgui_id, target);
    if (pin_focus) focus = target;
    else focus += (target - focus) * std::min(1.0f, ctx.IO.DeltaTime * 16.0f);
    if (std::abs(target - focus) < 0.01f) focus = target;
    const float t = focus;
    const auto& info = item.info;

    // Focused: the card fills the row and grows 8 px; resting: indented and shorter.
    const ImRect card_bb{{outer_pos.x + (1.0f - t) * 8.0f * S, outer_pos.y + (6.0f - 4.0f * t) * S},
                         {outer_pos.x + avail_w - (1.0f - t) * 8.0f * S, outer_pos.y + (6.0f - 4.0f * t) * S + base_card_h + 8.0f * S * t}};
    auto* draw = window.DrawList;

    Gow3::UI::DrawFocusCard(draw, card_bb.Min, card_bb.Max, t, S);

    // Thumbnail
    const float pad = (6.0f + t) * S;
    const float thumb_h = card_bb.GetHeight() - pad * 2.0f;
    const float thumb_w = thumb_h * (16.0f / 9.0f);
    const ImVec2 thumb_min{card_bb.Min.x + pad + 6.0f * S, card_bb.Min.y + pad};
    const ImVec2 thumb_max{thumb_min.x + thumb_w, thumb_min.y + thumb_h};

    if (item.icon) {
        draw->AddImage(item.icon.GetTexture().im_id, thumb_min, thumb_max);
    } else {
        draw->AddRectFilled(thumb_min, thumb_max, IM_COL32(18, 5, 5, 255), 2.0f);
        if (item.is_new) DrawPlus(draw, (thumb_min + thumb_max) * 0.5f, thumb_h * 0.30f, Fade(Gow3::UI::Theme::GoldLight, 0.55f + 0.45f * t));
    }
    draw->AddRect(thumb_min, thumb_max, Fade(IM_COL32(201, 158, 75, 90), 1.0f - t), 2.0f, 0, 1.0f);
    draw->AddRect(thumb_min, thumb_max, Fade(Gow3::UI::Theme::GoldLight, t), 2.0f, 0, 1.6f);

    // Text Information
    auto* font = Gow3::UI::g_font_cinzel ? Gow3::UI::g_font_cinzel : ImGui::GetFont();
    const float text_x = thumb_max.x + 16.0f * S;
    const float text_y = card_bb.Min.y + pad + 4.0f * S;

    // Title (Uppercase)
    std::string upper_title = info.display_title;
    for (char& c : upper_title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const float title_font_size = (23.0f + 4.0f * t) * S;
    if (t > 0.0f) draw->AddText(font, title_font_size, {text_x + 1.0f, text_y + 1.0f}, Fade(IM_COL32(0, 0, 0, 200), t), upper_title.c_str());
    draw->AddText(font, title_font_size, {text_x, text_y}, ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(Gow3::UI::Theme::TextMain), ImVec4{1, 1, 1, 1}, t)), upper_title.c_str());

    // Subtitle: kind of save, difficulty and play time, as far as the save says
    const float sub_font_size = (19.0f + 2.0f * t) * S;
    const float sub_y = text_y + title_font_size + 6.0f * S;

    std::string sub_text;
    if (item.is_new) {
        sub_text = "Empty slot";
    } else if (item.is_corrupted) {
        sub_text = "Corrupted Save File";
    } else {
        sub_text = info.is_auto_save ? "Auto Save" : "Manual Save";
        if (!info.difficulty.empty()) sub_text += " · Difficulty: " + info.difficulty;
        if (!info.play_time.empty()) sub_text += " · " + info.play_time;
    }

    const ImU32 sub_color = item.is_corrupted ? IM_COL32(239, 68, 68, 255)
                          : ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(Gow3::UI::Theme::TextMuted), ImGui::ColorConvertU32ToFloat4(Gow3::UI::Theme::GoldLight), t));
    draw->AddText(font, sub_font_size, {text_x, sub_y}, sub_color, sub_text.c_str());
}

void SaveDialogUi::DrawList(bool interactive) {
    auto& ctx = *GetCurrentContext();
    const ImVec2 display_size = ctx.IO.DisplaySize;
    auto* list = GetWindowDrawList();
    auto* font = Gow3::UI::g_font_cinzel ? Gow3::UI::g_font_cinzel : ImGui::GetFont();

    const float cqw = std::min(display_size.x / 100.0f, display_size.y / 56.25f);
    // The screen is laid out in 100 cqw of width; a wider display (ultrawide) keeps that block centered.
    const float layout_w = std::min(display_size.x, 100.0f * cqw);
    const float layout_x = (display_size.x - layout_w) * 0.5f;
    const float pad_x = layout_x + 5.5f * cqw;
    const float right_edge = layout_x + layout_w - 5.5f * cqw;
    const float pad_y = 3.5f * cqw;

    // 1. Header: Golden Omega (Ω) + Title (No "Main Campaign")
    const float omega_r = std::max(16.0f, 1.4f * cqw);
    const float omega_cx = pad_x + omega_r;
    const float omega_cy = pad_y + omega_r;

    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.1f);
    Gow3::UI::DrawOmegaSymbol(list, {omega_cx, omega_cy}, omega_r, Gow3::UI::Theme::GoldBronze, pulse, true);

    const char* header_title = state->type == DialogType::SAVE   ? "SAVE GAME"
                             : state->type == DialogType::DELETE ? "DELETE SAVE"
                             : "LOAD GAME";

    const float header_font_size = std::max(22.0f, 2.0f * cqw);
    const float title_x = omega_cx + omega_r + 1.2f * cqw;
    const float title_y = omega_cy - header_font_size * 0.5f;

    list->AddText(font, header_font_size, {title_x + 1.5f, title_y + 1.5f}, IM_COL32(0, 0, 0, 220), header_title);
    list->AddText(font, header_font_size, {title_x, title_y}, Gow3::UI::Theme::GoldLight, header_title);

    if (state->type == DialogType::LOAD) {
        // The EBOOT patch allows 64 manual slots; only the original AUTOSAVE and USERDATA are reserved.
        const auto used = std::ranges::count_if(state->save_list, [](const auto& item) { return item.dir_name != "AUTOSAVE" && item.dir_name != "USERDATA"; });
        const auto counter = fmt::format("{}/64 SAVES", used);
        const float size = header_font_size * 0.65f;
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, counter.c_str());
        list->AddText(font, size, {right_edge - extent.x, omega_cy - extent.y * 0.5f}, Gow3::UI::Theme::GoldMuted, counter.c_str());
    }

    // 2. Greek Meander Divider Ribbon (True classic, uniform spacing, zero gaps, zero overlap)
    const float meander_w = right_edge - pad_x;
    const float meander_h = std::max(10.0f, 1.15f * cqw);
    const float meander_y = pad_y + 3.4f * cqw;
    Gow3::UI::DrawGreekMeander(list, {pad_x, meander_y}, meander_w, meander_h, Gow3::UI::Theme::GoldBronze, 1.8f);


    // 3. Grid Geometry
    const float grid_top = meander_y + meander_h + 1.6f * cqw;
    const float footer_h = 4.2f * cqw;
    const float grid_h = display_size.y - grid_top - footer_h;

    const float left_w = 55.0f * cqw;
    const float right_x = pad_x + left_w + 3.2f * cqw;
    const float right_w = right_edge - right_x;

    const int first_save = state->new_item.has_value() ? 1 : 0;
    const int total_items = first_save + static_cast<int>(state->save_list.size());

    // 4. Initial Focus logic
    const bool initial_focus = interactive && first_render && total_items > 0;
    if (initial_focus) {
        focused_idx = 0;
        const auto& saves = state->save_list;
        if (const auto* pos = std::get_if<FocusPos>(&state->focus_pos)) {
            if (*pos == FocusPos::DATAHEAD) focused_idx = first_save;
            else if (*pos == FocusPos::LISTTAIL || *pos == FocusPos::DATATAIL) focused_idx = total_items - 1;
            else if (*pos == FocusPos::DATALTATEST || *pos == FocusPos::DATAOLDEST) {
                const auto cmp = [oldest = *pos == FocusPos::DATAOLDEST](auto a, auto b) { return oldest ? a < b : a > b; };
                const auto save = std::ranges::min_element(saves, cmp, &SaveDialogState::Item::last_write);
                if (save != saves.end()) focused_idx = first_save + static_cast<int>(save - saves.begin());
            }
        } else if (const auto* dir_name = std::get_if<std::string>(&state->focus_pos); dir_name && !dir_name->empty()) {
            const auto save = std::ranges::find(saves, *dir_name, &SaveDialogState::Item::dir_name);
            if (save != saves.end()) focused_idx = first_save + static_cast<int>(save - saves.begin());
        }
    }

    focused_idx = std::clamp(focused_idx, 0, std::max(0, total_items - 1));

    // 5. Left Column: Scrollable Save Slot List
    SetCursorPos({pad_x, grid_top});
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f * UiScale(), 8.0f * UiScale()});
    PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::max(10.0f, 0.85f * cqw));
    PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 2.0f);
    PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(20, 12, 10, 200));
    PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(131, 102, 57, 230));
    PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(177, 140, 73, 245));
    PushStyleColor(ImGuiCol_ScrollbarGrabActive, Gow3::UI::Theme::GoldBronze);
    const bool pin_focus = total_items > 0 && (initial_focus || repin_focus);
    if (pin_focus) {
        // Set the scroll before drawing so Cross cannot confirm a row hidden on the opening frame.
        const float row_h = SaveRowHeight * UiScale(), spacing = GetStyle().ItemSpacing.y;
        const float pitch = std::floor(row_h + spacing); // ItemSize aligns each row to pixels
        SetNextWindowContentSize({0.0f, total_items * pitch - spacing});
        SetNextWindowScroll({-1.0f, std::max(0.0f, focused_idx * pitch + (row_h - grid_h) * 0.5f)});
    }
    repin_focus = false;
    BeginChild("##SaveSlotScroll", {left_w, grid_h}, ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (!interactive && !pin_focus && total_items > 0) SetItemCurrentNavFocus(GetID(focused_idx));
    if (total_items == 0) {
        auto* body = Gow3::UI::g_font_body ? Gow3::UI::g_font_body : font;
        const float size = std::max(14.0f, 1.1f * cqw);
        const char* none = "No saved data";
        const ImVec2 extent = body->CalcTextSizeA(size, FLT_MAX, 0.0f, none);
        GetWindowDrawList()->AddText(body, size, GetWindowPos() + ImVec2{(left_w - extent.x) * 0.5f, grid_h * 0.4f}, Gow3::UI::Theme::TextMuted, none);
    }
    int item_idx = 0;
    if (state->new_item.has_value()) {
        DrawItem(item_idx++, state->new_item.value(), interactive, pin_focus);
    }
    for (const auto& item : state->save_list) {
        DrawItem(item_idx++, item, interactive, pin_focus);
    }
    EndChild();
    PopStyleColor(4);
    PopStyleVar(3);

    // 6. Right Column: Preview Slab
    const SaveDialogState::Item* selected_item = nullptr;
    if (state->new_item.has_value()) {
        if (focused_idx == 0) {
            selected_item = &state->new_item.value();
        } else if (focused_idx - 1 < static_cast<int>(state->save_list.size())) {
            selected_item = &state->save_list[focused_idx - 1];
        }
    } else if (focused_idx < static_cast<int>(state->save_list.size())) {
        selected_item = &state->save_list[focused_idx];
    }

    if (selected_item) {
        const auto& info = selected_item->info;

        // Preview thumbnail
        const float pv_thumb_w = right_w;
        const float pv_thumb_h = pv_thumb_w * (9.0f / 16.0f);
        const ImVec2 pv_thumb_pos{right_x, grid_top};

        if (selected_item->icon) {
            auto texture = selected_item->icon.GetTexture();
            list->AddImage(texture.im_id, pv_thumb_pos, pv_thumb_pos + ImVec2{pv_thumb_w, pv_thumb_h});
        } else {
            list->AddRectFilled(pv_thumb_pos, pv_thumb_pos + ImVec2{pv_thumb_w, pv_thumb_h}, IM_COL32(18, 5, 5, 255), 3.0f);
            if (selected_item->is_new) DrawPlus(list, pv_thumb_pos + ImVec2{pv_thumb_w, pv_thumb_h} * 0.5f, pv_thumb_h * 0.16f, Fade(Gow3::UI::Theme::GoldLight, 0.8f));
        }

        // Gold border & glow around large preview
        list->AddRect(pv_thumb_pos, pv_thumb_pos + ImVec2{pv_thumb_w, pv_thumb_h}, Gow3::UI::Theme::GoldBronze, 3.0f, 0, 2.0f);

        // Preview Title (Uppercase)
        std::string pv_title = info.clean_title;
        for (char& c : pv_title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        const float pv_title_size = std::max(18.0f, 1.45f * cqw);
        const float pv_title_y = pv_thumb_pos.y + pv_thumb_h + 1.2f * cqw;

        list->AddText(font, pv_title_size, {right_x + 1.0f, pv_title_y + 1.0f}, IM_COL32(0, 0, 0, 220), pv_title.c_str());
        list->AddText(font, pv_title_size, {right_x, pv_title_y}, Gow3::UI::Theme::GoldLight, pv_title.c_str());

        // Thin gold divider
        const float div_y = pv_title_y + pv_title_size + 0.8f * cqw;
        list->AddLine({right_x, div_y}, {right_x + right_w, div_y}, IM_COL32(201, 158, 75, 55), 1.0f);

        // Metadata Table (FOLDER, DIFFICULTY, PLAY TIME / Relog, DATE SAVED, FILE SIZE)
        float row_y = div_y + 1.0f * cqw;
        const float lbl_size = std::max(15.0f, 1.08f * cqw);
        const float row_h = lbl_size * 2.0f;
        const float val_x = right_x + right_w * 0.42f;
        const float val_width = right_edge - val_x;
        auto* body = Gow3::UI::g_font_body ? Gow3::UI::g_font_body : font;

        // Only what the save tells is listed.
        const std::pair<const char*, const std::string*> stats[] = {
            {"DIFFICULTY", &info.difficulty},
            {"PLAY TIME",  &info.play_time},
            {"DATE SAVED", &info.date_str},
            {"FILE SIZE",  &info.size_str},
        };
        const std::string corrupted = "Corrupted";

        const auto add_row = [&](const char* label, const std::string& value, ImU32 color) {
            list->AddText(body, lbl_size, {right_x, row_y}, Gow3::UI::Theme::GoldMuted, label);
            list->AddText(body, lbl_size, {val_x, row_y}, color, value.c_str(), nullptr, val_width);
            row_y += std::max(row_h, body->CalcTextSizeA(lbl_size, FLT_MAX, val_width, value.c_str()).y + lbl_size * 0.35f);
        };
        if (!selected_item->dir_name.empty()) add_row("FOLDER", selected_item->dir_name, Gow3::UI::Theme::GoldLight);
        if (selected_item->is_corrupted) add_row("CONDITION", corrupted, IM_COL32(239, 68, 68, 255));
        for (const auto& [label, value] : stats)
            if (!value->empty()) add_row(label, *value, Gow3::UI::Theme::TextMain);
    }

    // 7. Footer: Authentic PlayStation 4 Vector Buttons (Cross SELECT, Circle BACK)
    const float footer_y = display_size.y - 2.8f * cqw;
    const float btn_radius = std::max(11.0f, 0.95f * cqw);
    const float btn_font_size = std::max(15.0f, 1.06f * cqw);

    // Duplicating and deleting are offered on the saves of the save and load lists, not on the empty slot.
    const bool can_edit = selected_item && !selected_item->is_new && state->type != DialogType::DELETE;
    enum Action { ActDuplicate, ActDelete, ActSelect, ActBack };
    struct Hint { Gow3::UI::PS4Symbol symbol; const char* label; Action action; };
    Hint footer[4];
    int count = 0;
    if (can_edit) {
        footer[count++] = {Gow3::UI::PS4Symbol::Triangle, "DUPLICATE", ActDuplicate};
        footer[count++] = {Gow3::UI::PS4Symbol::Square, "DELETE", ActDelete};
    }
    footer[count++] = {Gow3::UI::PS4Symbol::Cross, selected_item && selected_item->is_new ? "CREATE" : "SELECT", ActSelect};
    footer[count++] = {Gow3::UI::PS4Symbol::Circle, "BACK", ActBack};

    // Laid out from the right edge inwards
    ImRect rects[4];
    float right = right_edge;
    for (int i = count - 1; i >= 0; --i) {
        const float left = right - Gow3::UI::HintWidth(footer[i].symbol, btn_radius, footer[i].label, font, btn_font_size);
        Gow3::UI::DrawHint(list, left, footer_y, btn_radius, footer[i].symbol, footer[i].label, font, btn_font_size);
        rects[i] = ImRect{{left, footer_y - btn_radius}, {right, footer_y + btn_radius}};
        right = left - btn_font_size * 0.95f;
    }

    const bool has_folder = selected_item && !selected_item->dir_name.empty();
    ImRect folder_button;
    if (has_folder) {
        const bool pad = ImGui::Core::GamepadPrompts();
        const float h = btn_radius * 1.7f;
        const float w = pad ? Gow3::UI::PadGlyphWidth(Gow3::UI::PadGlyph::Options, h) : Gow3::UI::KeyCapWidth("F", h, font);
        const ImVec2 center{pad_x + w * 0.5f, footer_y};
        if (pad) Gow3::UI::DrawPadGlyph(list, center, h, Gow3::UI::PadGlyph::Options);
        else Gow3::UI::DrawKeyCap(list, center, h, "F", font);
        constexpr const char* label = "OPEN SAVE FOLDER";
        const float label_x = pad_x + w + btn_font_size * 0.55f;
        list->AddText(font, btn_font_size, {label_x, footer_y - btn_font_size * 0.55f}, Gow3::UI::Theme::GoldLight, label);
        folder_button = {{pad_x, footer_y - btn_radius},
                         {label_x + font->CalcTextSizeA(btn_font_size, FLT_MAX, 0.0f, label).x, footer_y + btn_radius}};
    }

    if (!interactive || first_render) return;

    if (has_folder && (IsKeyPressed(ImGuiKey_GamepadStart, false) || IsKeyPressed(ImGuiKey_F, false) ||
                       (IsMouseClicked(ImGuiMouseButton_Left) && folder_button.Contains(ctx.IO.MousePos)))) {
        auto path = SaveInstance::MakeDirSavePath(state->user_id, state->title_id, selected_item->dir_name);
        wchar_t windows[MAX_PATH]{};
        const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (!length || length >= MAX_PATH) return Gow3::Log("Could not find Explorer in the Windows directory");
        const auto explorer = std::filesystem::path(std::wstring_view(windows, length)) / L"explorer.exe";
        const auto args = std::wstring(L"/select,\"") + path.make_preferred().native() + L"\"";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", explorer.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
            Gow3::Log("Could not open the save folder in Explorer");
        return;
    }

    const auto cancel = [&] {
        result->dir_name.clear();
        Finish(ButtonId::INVALID, Result::USER_CANCELED);
    };
    // A new save is made at once; over an existing one in the save list the player is asked first.
    const auto select = [&] {
        if (!selected_item) return;
        if (state->type == DialogType::SAVE && !selected_item->is_new) {
            OpenPrompt(Prompt::Overwrite, focused_idx);
            return;
        }
        result->dir_name = selected_item->dir_name;
        result->param = selected_item->pfo;
        if (state->type == DialogType::SAVE) StartLinger("CREATE A NEW SAVE", "Saving...");
        Finish(ButtonId::INVALID);
    };
    const auto act = [&](Action action) {
        if (action == ActBack) cancel();
        else if (action == ActSelect) select();
        else if (can_edit) OpenPrompt(action == ActDelete ? Prompt::Delete : Prompt::Duplicate, focused_idx);
    };

    if (IsKeyPressed(ImGuiKey_GamepadFaceRight) || IsKeyPressed(ImGuiKey_Escape)) return act(ActBack);
    if (IsKeyPressed(ImGuiKey_GamepadFaceLeft) || IsKeyPressed(ImGuiKey_Delete)) return act(ActDelete);
    if (IsKeyPressed(ImGuiKey_GamepadFaceUp) || IsKeyPressed(ImGuiKey_Insert)) return act(ActDuplicate);
    if (activated >= 0 || IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || IsKeyPressed(ImGuiKey_Enter, false)) {
        activated = -1;
        return act(ActSelect);
    }
    if (IsMouseClicked(ImGuiMouseButton_Left)) {
        for (int i = 0; i < count; ++i) {
            if (rects[i].Contains(ctx.IO.MousePos)) return act(footer[i].action);
        }
    }
}

namespace {
// The message of a modal, centered above its buttons.
void ModalMessage(const std::string& msg, ImU32 color, float S) {
    if (msg.empty()) return;
    PushFont(Gow3::UI::g_font_body, 22.0f * S);
    PushStyleColor(ImGuiCol_Text, color);
    DrawCenteredText(msg.c_str(), msg.c_str() + msg.size(), GetContentRegionAvail() - ImVec2{0.0f, 50.0f * S});
    PopStyleColor();
    PopFont();
}

// The directory name of a copy of a save, at most 31 characters (what the game's directory names hold). An auto save is
// "AUTOSAVE - " and the place it was made in, else the time of the copy; any other save keeps its name. A name that is taken
// gets the first free " (n)", the way the game numbers repeated saves. Empty when none is free.
std::string CopyName(const SaveDialogState::Item& item, const auto& taken) {
    constexpr size_t max_length = 31;
    std::string base = item.dir_name;
    if (item.info.is_auto_save) {
        std::string place = item.info.clean_title;
        if (place.empty() || place == item.dir_name || place == "Save Game") {
            const std::time_t now = std::time(nullptr);
            place = fmt::format("{:%y%m%d-%H%M%S}", *std::localtime(&now));
        }
        for (char& c : place) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::erase_if(place, [](char c) { return c < 32 || std::string_view{"<>:\"/\\|?*"}.find(c) != std::string_view::npos; });
        base = "AUTOSAVE - " + place;
    }
    const auto fit = [&](const std::string& suffix) {
        std::string name = base.substr(0, max_length - suffix.size());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name + suffix;
    };
    if (item.info.is_auto_save && !taken(fit(""))) return fit("");
    for (int n = 2; n < 100; ++n) {
        if (const std::string name = fit(fmt::format(" ({})", n)); !taken(name)) return name;
    }
    return {};
}

// Buttons of a modal: the display's scale, the title font, a gold edge.
struct ModalButtons {
    explicit ModalButtons(float S, int count) : S(S), size(120.0f * S, 34.0f * S) {
        const float width = size.x * count + 20.0f * S * (count - 1);
        SetCursorPos({(GetWindowSize().x - width) * 0.5f, GetWindowSize().y - 50.0f * S});
        BeginGroup();
        PushFont(Gow3::UI::g_font_cinzel, 20.0f * S);
        PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        PushStyleColor(ImGuiCol_Text, Gow3::UI::Theme::GoldLight);
    }
    ~ModalButtons() {
        PopStyleColor();
        PopStyleVar();
        PopFont();
        EndGroup();
    }
    bool Button(const char* label, bool focus = false, bool same_line = false) {
        if (same_line) SameLine(0.0f, 20.0f * S);
        if (focus) SetKeyboardFocusHere();
        return ImGui::Button(label, size);
    }
    float S;
    ImVec2 size;
};
} // namespace

// Circle or Escape on a modal: the answer that backs out of it (false when it cannot be left).
bool SaveDialogUi::CancelModal() {
    switch (state->GetMode()) {
    case SaveDataDialogMode::USER_MSG:
        switch (state->GetState<SaveDialogState::UserState>().type) {
        case ButtonType::YESNO: Finish(ButtonId::NO); return true;
        case ButtonType::OKCANCEL: Finish(ButtonId::INVALID, Result::USER_CANCELED); return true;
        case ButtonType::OK: Finish(ButtonId::OK); return true;
        default: return false;
        }
    case SaveDataDialogMode::SYSTEM_MSG: {
        const auto& sys = state->GetState<SaveDialogState::SystemState>();
        if (sys.show_no) Finish(ButtonId::NO);
        else if (sys.show_cancel) Finish(ButtonId::INVALID, Result::USER_CANCELED);
        else if (!sys.hide_ok) Finish(ButtonId::OK);
        else return false;
        return true;
    }
    case SaveDataDialogMode::ERROR_CODE: Finish(ButtonId::OK); return true;
    default: return false;
    }
}

void SaveDialogUi::DrawUser() {
    const auto& user_state = state->GetState<SaveDialogState::UserState>();
    const auto btn_type = user_state.type;
    const float S = UiScale();

    if (!state->save_list.empty()) {
        DrawItem(0, state->save_list.front(), false);
    } else if (state->new_item) {
        DrawItem(0, *state->new_item, false);
    }

    ModalMessage(user_state.msg, user_state.msg_type == UserMessageType::ERROR ? IM_COL32(255, 51, 51, 255) : Gow3::UI::Theme::TextMain, S);

    if (btn_type != ButtonType::NONE) {
        const bool two = btn_type == ButtonType::YESNO || btn_type == ButtonType::OKCANCEL;
        ModalButtons buttons(S, two ? 2 : 1);
        if (btn_type == ButtonType::YESNO) {
            if (buttons.Button("Yes")) Finish(ButtonId::YES);
            // Deleting asks with "No" ready, so a stray Cross cannot destroy a save.
            if (buttons.Button("No", state->type == DialogType::DELETE && first_render, true)) Finish(ButtonId::NO);
        } else {
            if (buttons.Button("OK")) Finish(ButtonId::OK);
            if (btn_type == ButtonType::OKCANCEL && buttons.Button("Cancel", false, true)) Finish(ButtonId::INVALID, Result::USER_CANCELED);
        }
    }
}

void SaveDialogUi::DrawSystemMessage() {
    const auto& sys_state = state->GetState<SaveDialogState::SystemState>();
    const float S = UiScale();

    if (!state->save_list.empty()) {
        DrawItem(0, state->save_list.front(), false);
    } else if (state->new_item) {
        DrawItem(0, *state->new_item, false);
    }

    ModalMessage(sys_state.msg, Gow3::UI::Theme::TextMain, S);

    const bool extra = sys_state.show_no || sys_state.show_cancel;
    const int count = (sys_state.hide_ok ? 0 : 1) + (extra ? 1 : 0);
    if (count == 0) return;
    ModalButtons buttons(S, count);
    if (!sys_state.hide_ok && buttons.Button(sys_state.show_no ? "Yes" : "OK")) Finish(ButtonId::YES);
    if (sys_state.show_no) {
        // Deleting or overwriting asks with "No" ready, so a stray Cross cannot destroy a save.
        const bool destructive = state->type == DialogType::DELETE || sys_state.type == SystemMessageType::OVERWRITE;
        if (buttons.Button("No", destructive && first_render, !sys_state.hide_ok)) Finish(ButtonId::NO);
    } else if (sys_state.show_cancel) {
        if (buttons.Button("Cancel", false, !sys_state.hide_ok)) Finish(ButtonId::INVALID, Result::USER_CANCELED);
    }
}

void SaveDialogUi::DrawErrorCode() {
    const auto& err_state = state->GetState<SaveDialogState::ErrorCodeState>();
    const float S = UiScale();

    if (!state->save_list.empty()) {
        DrawItem(0, state->save_list.front(), false);
    } else if (state->new_item) {
        DrawItem(0, *state->new_item, false);
    }

    ModalMessage(err_state.error_msg, Gow3::UI::Theme::TextMain, S);

    ModalButtons buttons(S, 1);
    if (buttons.Button("OK")) Finish(ButtonId::OK);
}

void SaveDialogUi::DrawProgressBar() {
    const auto& bar_state = state->GetState<SaveDialogState::ProgressBarState>();
    const auto ws = GetWindowSize();
    const float S = UiScale();
    auto* list = GetWindowDrawList();

    if (!state->save_list.empty()) {
        DrawItem(0, state->save_list.front(), false);
    } else if (state->new_item) {
        DrawItem(0, *state->new_item, false);
    }

    ModalMessage(bar_state.msg, Gow3::UI::Theme::TextMain, S);

    // Clean Arterial Blood Trench progress bar (the draw list takes screen coordinates)
    const float bar_w = ws.x * 0.70f;
    const float bar_h = 16.0f * S;
    const float bar_x = GetWindowPos().x + (ws.x - bar_w) * 0.5f;
    const float bar_y = GetWindowPos().y + ws.y - 50.0f * S;

    list->AddRectFilledMultiColor({bar_x, bar_y}, {bar_x + bar_w, bar_y + bar_h},
        Gow3::UI::Theme::BloodTrenchDark, Gow3::UI::Theme::BloodTrenchDark,
        Gow3::UI::Theme::BloodTrenchMid, Gow3::UI::Theme::BloodTrenchMid);

    const float fill_w = bar_w * std::clamp(static_cast<float>(bar_state.progress) / 100.0f, 0.0f, 1.0f);
    if (fill_w > 1.0f) {
        list->AddRectFilledMultiColor({bar_x, bar_y}, {bar_x + fill_w, bar_y + bar_h},
            Gow3::UI::Theme::BloodCore, Gow3::UI::Theme::BloodCore,
            Gow3::UI::Theme::BloodDeep, Gow3::UI::Theme::BloodDeep);
    }
    list->AddRect({bar_x, bar_y}, {bar_x + bar_w, bar_y + bar_h}, Gow3::UI::Theme::GoldBronze, 1.0f, 0, 1.5f);
}

// A notice over the list: a title, the message under it wrapped and centered, then Back (and Confirm, when it asks).
// `take_input` is off on the frame the notice opens, so the press that opened it cannot answer it.
// With `busy` a spinner takes the place of the buttons.
SaveDialogUi::Answer SaveDialogUi::DrawNotice(const ImGuiIO& io, const char* title, const std::string& message, bool confirm, bool take_input, bool busy) {
    auto* overlay = GetForegroundDrawList();
    overlay->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(12, 10, 10, 220));
    auto* font = Gow3::UI::g_font_cinzel ? Gow3::UI::g_font_cinzel : GetFont();
    auto* body_font = Gow3::UI::g_font_body ? Gow3::UI::g_font_body : font;
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;

    const float title_size = std::max(20.0f, H * 0.045f);
    const ImVec2 text_size = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title);
    overlay->AddText(font, title_size, {(W - text_size.x) * 0.5f, H * 0.42f - text_size.y * 0.5f}, Gow3::UI::Theme::TextMain, title);

    const float body_size = std::max(14.0f, H * 0.024f), wrap_width = W * 0.7f;
    float y = H * 0.42f + text_size.y * 0.5f + body_size;
    const char* end = message.c_str() + message.size();
    for (const char* s = message.c_str(); s < end;) {
        const char* next = body_font->CalcWordWrapPosition(body_size, s, end, wrap_width);
        if (next <= s) next = s + 1;
        const char* trim = next;
        while (trim > s && (trim[-1] == ' ' || trim[-1] == '\n')) --trim;
        const float line_w = body_font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, s, trim).x;
        overlay->AddText(body_font, body_size, {(W - line_w) * 0.5f, y}, Gow3::UI::Theme::TextMuted, s, trim);
        y += body_size * 1.35f;
        for (s = next; s < end && (*s == ' ' || *s == '\n'); ++s) {}
    }

    // The buttons side by side, the group centered
    const float button_size = std::max(13.0f, H * 0.022f);
    const float radius = button_size * 0.8f;
    const float button_y = std::max(H * 0.62f, y + radius + body_size);
    if (busy) { // no progress to show, only that it is working: a gold tail ending in a blood-red head, round the Omega
        const float spin_r = button_size * 1.2f;
        const float thickness = std::max(2.0f, spin_r * 0.14f);
        const ImVec2 center{W * 0.5f, button_y};
        const float time = static_cast<float>(GetTime());
        overlay->AddCircle(center, spin_r, IM_COL32(201, 158, 75, 45), 48, 1.0f);
        constexpr int pieces = 14;
        constexpr float tail = 2.4f;  // radians
        const float head = time * 4.2f;
        for (int i = 0; i < pieces; ++i) {  // the oldest piece first, fading towards the tail's end
            const float from = head - tail + tail * i / pieces, to = head - tail + tail * (i + 1) / pieces + 0.02f;
            const float alpha = static_cast<float>(i + 1) / pieces;
            overlay->PathArcTo(center, spin_r, from, to, 4);
            overlay->PathStroke(Fade(Gow3::UI::Theme::GoldLight, alpha * alpha), 0, thickness);
        }
        overlay->AddCircleFilled({center.x + std::cos(head) * spin_r, center.y + std::sin(head) * spin_r}, thickness * 0.9f, Gow3::UI::Theme::BloodCore);
        Gow3::UI::DrawOmegaSymbol(overlay, center, spin_r * 0.5f, Gow3::UI::Theme::GoldBronze, 0.5f + 0.5f * std::sin(time * 2.1f), false);
        return Answer::None;
    }
    struct Choice { Gow3::UI::PS4Symbol symbol; const char* label; Answer answer; float width; ImRect rect; };
    Choice choices[2] = {{Gow3::UI::PS4Symbol::Cross, "CONFIRM", Answer::Confirm, 0.0f, {}}, {Gow3::UI::PS4Symbol::Circle, "BACK", Answer::Back, 0.0f, {}}};
    Choice* shown = confirm ? choices : choices + 1;
    const int count = confirm ? 2 : 1;
    const float gap = button_size * 3.0f;
    float total = gap * (count - 1);
    for (int i = 0; i < count; ++i) {
        shown[i].width = Gow3::UI::HintWidth(shown[i].symbol, radius, shown[i].label, font, button_size);
        total += shown[i].width;
    }
    float x = (W - total) * 0.5f;
    for (int i = 0; i < count; ++i) {
        Gow3::UI::DrawHint(overlay, x, button_y, radius, shown[i].symbol, shown[i].label, font, button_size);
        shown[i].rect = ImRect{{x, button_y - radius}, {x + shown[i].width, button_y + radius}};
        x += shown[i].width + gap;
    }

    if (!take_input) return Answer::None;
    if (IsKeyPressed(ImGuiKey_GamepadFaceRight) || IsKeyPressed(ImGuiKey_Escape)) return Answer::Back;
    if (confirm && (IsKeyPressed(ImGuiKey_GamepadFaceDown) || IsKeyPressed(ImGuiKey_Enter))) return Answer::Confirm;
    if (IsMouseClicked(ImGuiMouseButton_Left)) {
        for (int i = 0; i < count; ++i) {
            if (shown[i].rect.Contains(io.MousePos)) return shown[i].answer;
        }
    }
    return Answer::None;
}

// A load that cannot happen: the title says it failed, the subtitle says why (no saved data included).
void SaveDialogUi::DrawLoadFailed(const ImGuiIO& io) {
    std::string reason;
    if (state->mode == SaveDataDialogMode::ERROR_CODE) reason = state->GetState<SaveDialogState::ErrorCodeState>().error_msg;
    else if (state->mode == SaveDataDialogMode::USER_MSG) reason = state->GetState<SaveDialogState::UserState>().msg;
    else reason = state->GetState<SaveDialogState::SystemState>().msg;
    if (reason.empty()) reason = "The saved data could not be loaded.";
    if (DrawNotice(io, "LOADING FAILED", reason, false, true) == Answer::Back) Finish(ButtonId::OK);
}

void SaveDialogUi::OpenPrompt(Prompt kind, int index) {
    prompt = kind;
    prompt_idx = index;
    prompt_frame = GetFrameCount();
}

// The question over the list: overwrite the save, or delete it.
void SaveDialogUi::DrawPrompt(const ImGuiIO& io) {
    const bool failed = prompt == Prompt::Failed;
    const char* title = failed ? failure_title : prompt == Prompt::Delete ? "DELETE SAVE" : prompt == Prompt::Duplicate ? "DUPLICATE SAVE" : "OVERWRITE SAVE";
    const std::string message = failed                       ? failure
                              : prompt == Prompt::Delete     ? "Do you wish to delete the save?"
                              : prompt == Prompt::Duplicate  ? "Do you wish to duplicate the save?"
                                                             : "Are you sure you want to overwrite this save?";
    const Answer answer = DrawNotice(io, title, message, !failed, GetFrameCount() > prompt_frame);
    if (answer == Answer::None) return;
    if (answer == Answer::Back) {
        prompt = Prompt::None;
        return;
    }

    const int index = prompt_idx - (state->new_item ? 1 : 0);
    if (index < 0 || index >= static_cast<int>(state->save_list.size())) {
        prompt = Prompt::None;
        return;
    }
    const auto& item = state->save_list[index];
    const auto path_of = [&](const std::string& dir_name) { return SaveInstance::MakeDirSavePath(state->user_id, state->title_id, dir_name); };
    const auto fail = [&](const char* what, std::string why) {
        prompt = Prompt::Failed, failure_title = what, failure = std::move(why);
    };

    if (prompt == Prompt::Overwrite) { // the game gets the save to write over
        result->dir_name = item.dir_name;
        result->param = item.pfo;
        prompt = Prompt::None;
        overwrite_confirmed = true;
        StartLinger(title, message); // the question stays up with a spinner until the game is done writing
        Finish(ButtonId::INVALID);
        return;
    }

    std::error_code error;
    if (prompt == Prompt::Delete) {
        std::filesystem::remove_all(path_of(item.dir_name), error);
        if (error) {
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save delete failed directory={} error={}", item.dir_name, error.message()));
            return fail("DELETE SAVE", "The saved data could not be deleted.");
        }
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save deleted directory={}", item.dir_name));
        state->save_list.erase(state->save_list.begin() + index);
        focused_idx = prompt_idx;
    } else { // Duplicate: the folder copied under a new name, which the list then shows and focuses
        const std::string source = item.dir_name;
        const std::string name = CopyName(item, [&](const std::string& candidate) { return std::filesystem::exists(path_of(candidate)); });
        const auto from = path_of(source), to = path_of(name);
        if (name.empty()) return fail("DUPLICATE SAVE", "The saved data could not be duplicated.");
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, error);
        if (!error) { // the copy keeps the dates of the original
            for (const auto& entry : std::filesystem::recursive_directory_iterator(from, error)) {
                std::error_code ignored;
                std::filesystem::last_write_time(to / std::filesystem::relative(entry.path(), from), std::filesystem::last_write_time(entry.path(), ignored), ignored);
            }
        }
        if (error || !state->LoadItem(name)) {
            GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save duplicate failed directory={} error={}", source, error.message()));
            std::filesystem::remove_all(to, error);
            return fail("DUPLICATE SAVE", "The saved data could not be duplicated.");
        }
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save duplicated directory={} as={}", source, name));
        focused_idx = (state->new_item ? 1 : 0) + static_cast<int>(state->save_list.size()) - 1;
    }
    repin_focus = true;
    prompt = Prompt::None;
}

// The chosen save's screen kept up while the game writes it: the list behind, the question over it, a spinner under.
void SaveDialogUi::DrawLingering(const ImGuiIO& io) {
    SetNextWindowPos({0, 0});
    SetNextWindowSize(io.DisplaySize);
    SetNextWindowCollapsed(false);
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    if (Begin("Save Data Dialog##SaveDataDialog", nullptr,
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        Gow3::UI::DrawBasaltBackground(GetWindowDrawList(), {0, 0}, io.DisplaySize);
        auto* current = state;
        const int current_focus = focused_idx;
        const bool current_first = first_render;
        state = &*linger;
        focused_idx = linger_focus;
        first_render = false;
        DrawList(false);
        state = current;
        focused_idx = current_focus;
        first_render = current_first;
        DrawNotice(io, linger_title.c_str(), linger_message, false, false, true);
    }
    End();
    PopStyleColor();
    PopStyleVar(2);
}

void SaveDialogUi::Draw() {
    std::unique_lock lock{draw_mutex};

    if (linger_active) {
        const auto& io = GetCurrentContext()->IO;
        if (std::chrono::steady_clock::now() > linger_deadline) { // the game opened nothing more
            EndLinger();
            RemoveLayer(this);
        } else {
            DrawLingering(io);
        }
        if (status && *status == Status::FINISHED) { // the dialog the game closed is done with, as at the end of a normal draw
            if (state) *state = SaveDialogState{};
            state = nullptr;
            status = nullptr;
            result = nullptr;
        }
        return;
    }

    if (status == nullptr || *status != Status::RUNNING || state == nullptr) {
        return;
    }

    if (state->mode == SaveDataDialogMode::PROGRESS_BAR && state->type == DialogType::LOAD) {
        return;
    }

    if (first_render) {
        GOW3_DIAG Gow3::Log(fmt::format("DIALOG_TRACE save draw mode={}", static_cast<u32>(state->mode)));
    }

    const auto& ctx = *GetCurrentContext();
    const auto& io = ctx.IO;

    const bool is_list = (state->GetMode() == SaveDataDialogMode::LIST);
    const bool load_error = state->type == DialogType::LOAD &&
        (state->mode == SaveDataDialogMode::ERROR_CODE ||
         (state->mode == SaveDataDialogMode::USER_MSG &&
          state->GetState<SaveDialogState::UserState>().msg_type == UserMessageType::ERROR &&
          state->GetState<SaveDialogState::UserState>().type == ButtonType::OK) ||
         (state->mode == SaveDataDialogMode::SYSTEM_MSG &&
          (state->GetState<SaveDialogState::SystemState>().type == SystemMessageType::NODATA ||
           state->GetState<SaveDialogState::SystemState>().type == SystemMessageType::FILE_CORRUPTED)));

    if (is_list || load_error) {
        // Fullscreen cinematic Spartan presentation
        SetNextWindowPos({0, 0});
        SetNextWindowSize(io.DisplaySize);
        SetNextWindowCollapsed(false);

        if (first_render || !io.NavActive) {
            SetNextWindowFocus();
        }

        PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

        if (Begin("Save Data Dialog##SaveDataDialog", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {

            // Refined dark basalt background with warm crimson radial underglow and subtle gold veins
            Gow3::UI::DrawBasaltBackground(GetWindowDrawList(), {0, 0}, io.DisplaySize);

            if (is_list) {
                DrawList(prompt == Prompt::None);
                if (prompt != Prompt::None && *status == Status::RUNNING) DrawPrompt(io);
            } else {
                if (last_load_list) {
                    auto* error_state = state;
                    const int error_focus = focused_idx;
                    const bool error_first_render = first_render;
                    state = &*last_load_list;
                    focused_idx = last_load_focus;
                    first_render = false;
                    DrawList(false);
                    state = error_state;
                    focused_idx = error_focus;
                    first_render = error_first_render;
                }

                DrawLoadFailed(io);
            }
        }
        End();

        PopStyleColor();
        PopStyleVar(2);
    } else {
        // Modal prompt box with basalt styling
        const ImVec2 window_size{io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.45f};
        CentralizeNextWindow();
        SetNextWindowSize(window_size);
        SetNextWindowCollapsed(false);

        if (first_render || !io.NavActive) {
            SetNextWindowFocus();
        }

        PushStyleColor(ImGuiCol_WindowBg, Gow3::UI::Theme::ObsidianBg);
        PushStyleColor(ImGuiCol_Border, Gow3::UI::Theme::GoldBronze);
        PushStyleColor(ImGuiCol_Button, Gow3::UI::Theme::PS4BtnBg);
        PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 20, 20, 255));
        PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 15, 15, 255));

        if (Begin("Save Data Dialog##SaveDataDialog", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings)) {

            // Meander line at top of modal
            const float m_w = GetContentRegionAvail().x;
            Gow3::UI::DrawGreekMeander(GetWindowDrawList(), GetCursorScreenPos(), m_w, 12.0f * UiScale(), Gow3::UI::Theme::GoldBronze, 1.5f);
            Dummy({0, 16.0f * UiScale()});

            BeginGroup();
            switch (state->GetMode()) {
            case SaveDataDialogMode::USER_MSG:
                DrawUser();
                break;
            case SaveDataDialogMode::SYSTEM_MSG:
                DrawSystemMessage();
                break;
            case SaveDataDialogMode::ERROR_CODE:
                DrawErrorCode();
                break;
            case SaveDataDialogMode::PROGRESS_BAR:
                DrawProgressBar();
                break;
            default:
                TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Unknown dialog mode");
            }
            EndGroup();
            // Circle or Escape backs out of the prompt, as on the console
            if (!first_render && *status == Status::RUNNING && (IsKeyPressed(ImGuiKey_GamepadFaceRight) || IsKeyPressed(ImGuiKey_Escape))) CancelModal();
        }
        End();

        PopStyleColor(5);
    }

    first_render = false;
    if (status && *status == Status::FINISHED) {
        if (state) {
            *state = SaveDialogState{};
        }
        state = nullptr;
        status = nullptr;
        result = nullptr;
    }
}

} // namespace Libraries::SaveData::Dialog
