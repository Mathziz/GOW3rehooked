// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "orbis/core/file_format/psf.h"
#include "orbis/internals/save_data/savedata.h"
#include "orbis/internals/system/commondialog.h"
#include "ui/imgui_layer.h"
#include "ui/imgui_texture.h"

namespace Libraries::SaveData::Dialog {

using OrbisUserServiceUserId = s32;

enum class SaveDataDialogMode : u32 {
    INVALID = 0,
    LIST = 1,
    USER_MSG = 2,
    SYSTEM_MSG = 3,
    ERROR_CODE = 4,
    PROGRESS_BAR = 5,
};

enum class DialogType : u32 {
    SAVE = 1,
    LOAD = 2,
    DELETE = 3,
};

enum class DialogAnimation : u32 {
    ON = 0,
    OFF = 1,
};

enum class ButtonId : u32 {
    INVALID = 0,
    OK = 1,
    YES = 1,
    NO = 2,
};

enum class ButtonType : u32 {
    OK = 0,
    YESNO = 1,
    NONE = 2,
    OKCANCEL = 3,
};

enum class UserMessageType : u32 {
    NORMAL = 0,
    ERROR = 1,
};

enum class FocusPos : u32 {
    LISTHEAD = 0,
    LISTTAIL = 1,
    DATAHEAD = 2,
    DATATAIL = 3,
    DATALTATEST = 4,
    DATAOLDEST = 5,
    DIRNAME = 6,
};

enum class ItemStyle : u32 {
    TITLE_DATASIZE_SUBTITLE = 0,
    TITLE_SUBTITLE_DATESIZE = 1,
    TITLE_DATESIZE = 2,
};

enum class SystemMessageType : u32 {
    NODATA = 1,
    CONFIRM = 2,
    OVERWRITE = 3,
    NOSPACE = 4,
    PROGRESS = 5,
    FILE_CORRUPTED = 6,
    FINISHED = 7,
    NOSPACE_CONTINUABLE = 8,
    CORRUPTED_AND_DELETED = 10,
    CORRUPTED_AND_CREATED = 11,
    CORRUPTED_AND_RESTORE = 13,
    TOTAL_SIZE_EXCEEDED = 14,
};

enum class ProgressBarType : u32 {
    PERCENTAGE = 0,
};

enum class ProgressSystemMessageType : u32 {
    INVALID = 0,
    PROGRESS = 1,
    RESTORE = 2,
};

enum class OptionBack : u32 {
    ENABLE = 0,
    DISABLE = 1,
};

enum class OrbisSaveDataDialogProgressBarTarget : u32 {
    DEFAULT = 0,
};

struct AnimationParam {
    DialogAnimation userOK;
    DialogAnimation userCancel;
    std::array<u8, 32> _reserved;
};

struct SaveDialogNewItem {
    const char* title;
    void* iconBuf;
    size_t iconSize;
    std::array<u8, 32> _reserved;
};

struct SaveDialogItems {
    OrbisUserServiceUserId userId;
    s32 : 32;
    const OrbisSaveDataTitleId* titleId;
    const OrbisSaveDataDirName* dirName;
    u32 dirNameNum;
    s32 : 32;
    const SaveDialogNewItem* newItem;
    FocusPos focusPos;
    s32 : 32;
    const OrbisSaveDataDirName* focusPosDirName;
    ItemStyle itemStyle;
    std::array<u8, 32> _reserved;
};

struct UserMessageParam {
    ButtonType buttonType;
    UserMessageType msgType;
    const char* msg;
    std::array<u8, 32> _reserved;
};

struct SystemMessageParam {
    SystemMessageType msgType;
    s32 : 32;
    u64 value;
    std::array<u8, 32> _reserved;
};

struct ErrorCodeParam {
    u32 errorCode;
    std::array<u8, 32> _reserved;
};

struct ProgressBarParam {
    ProgressBarType barType;
    s32 : 32;
    const char* msg;
    ProgressSystemMessageType sysMsgType;
    std::array<u8, 28> _reserved;
};

struct OptionParam {
    OptionBack back;
    std::array<u8, 32> _reserved;
};

struct OrbisSaveDataDialogParam {
    CommonDialog::BaseParam baseParam;
    s32 size;
    SaveDataDialogMode mode;
    DialogType dispType;
    s32 : 32;
    AnimationParam* animParam;
    SaveDialogItems* items;
    UserMessageParam* userMsgParam;
    SystemMessageParam* sysMsgParam;
    ErrorCodeParam* errorCodeParam;
    ProgressBarParam* progressBarParam;
    void* userData;
    OptionParam* optionParam;
    std::array<u8, 24> _reserved;
};

struct OrbisSaveDataDialogResult {
    SaveDataDialogMode mode{};
    CommonDialog::Result result{};
    ButtonId buttonId{};
    s32 : 32;
    OrbisSaveDataDirName* dirName;
    OrbisSaveDataParam* param;
    void* userData;
    std::array<u8, 32> _reserved;
};

struct SaveDialogResult {
    SaveDataDialogMode mode{};
    CommonDialog::Result result{CommonDialog::Result::OK};
    ButtonId button_id{ButtonId::INVALID};
    std::string dir_name{};
    PSF param{};
    void* user_data{};

    void CopyTo(OrbisSaveDataDialogResult& result) const;
};

// What the dialog shows of a save, read once from its param.sfo; an empty field is not shown.
struct SaveParsedInfo {
    std::string display_title;  // e.g. "(Auto Save) The City of Olympia"
    std::string clean_title;    // e.g. "The City of Olympia"
    std::string difficulty;
    std::string play_time;      // e.g. "02:02:30"
    std::string date_str;       // e.g. "02 Nov 2026 · 11:41 PM"
    std::string size_str;       // e.g. "1.27 MB"
    bool is_auto_save = false;
};

class SaveDialogState {
    friend class SaveDialogUi;

public:
    struct UserState {
        ButtonType type{};
        UserMessageType msg_type{};
        std::string msg{};

        UserState(const OrbisSaveDataDialogParam& param);
    };
    struct SystemState {
        SystemMessageType type{};
        std::string msg{};
        bool hide_ok{};
        bool show_no{}; // Yes instead of OK
        bool show_cancel{};

        bool return_cancel{};

        SystemState(const SaveDialogState& state, const OrbisSaveDataDialogParam& param);
    };
    struct ErrorCodeState {
        std::string error_msg{};

        ErrorCodeState(const OrbisSaveDataDialogParam& param);
    };
    struct ProgressBarState {
        std::string msg{};
        u32 progress{};

        ProgressBarState(const SaveDialogState& state, const OrbisSaveDataDialogParam& param);
    };

    struct Item {
        std::string dir_name{};
        ImGui::RefCountedTexture icon{};

        std::string title{};
        std::string subtitle{};
        std::string details{};
        std::string date{};
        std::string size{};

        std::chrono::system_clock::time_point last_write{};
        PSF pfo{};
        bool is_corrupted{};
        bool is_new{}; // the empty slot that creates a save
        SaveParsedInfo info{};
    };

private:
    SaveDataDialogMode mode{};
    DialogType type{};
    void* user_data{};
    std::optional<bool> enable_back{};

    OrbisUserServiceUserId user_id{};
    std::string title_id{};
    std::vector<Item> save_list{};
    std::variant<FocusPos, std::string, std::monostate> focus_pos{std::monostate{}};
    ItemStyle style{};

    std::optional<Item> new_item{};

    std::variant<UserState, SystemState, ErrorCodeState, ProgressBarState, std::monostate> state{
        std::monostate{}};

    bool LoadItem(std::string_view dir_name);

public:
    explicit SaveDialogState(const OrbisSaveDataDialogParam& param);

    SaveDialogState() = default;

    [[nodiscard]] SaveDataDialogMode GetMode() const {
        return mode;
    }

    template <typename T>
    [[nodiscard]] T& GetState() {
        return std::get<T>(state);
    }
};

class SaveDialogUi final : public ImGui::Layer {
    bool first_render{false};
    SaveDialogState* state{};
    CommonDialog::Status* status{};
    SaveDialogResult* result{};

    std::recursive_mutex draw_mutex{};
    int focused_idx{0};
    std::optional<SaveDialogState> last_load_list;
    int last_load_focus{0};
    bool overwrite_confirmed{false}; // the list asked about an overwrite; the game's next question about it is answered Yes
    // After a save is chosen the list and its question stay up with a spinner while the game writes it: the game's own
    // follow-up dialogs (its overwrite question, its progress box) are absorbed, and everything goes away together.
    bool linger_active{false};
    std::optional<SaveDialogState> linger;
    int linger_focus{0};
    std::string linger_title;
    std::string linger_message;
    std::chrono::steady_clock::time_point linger_deadline{};
    int activated{-1};         // the row clicked while in focus
    bool repin_focus{false};   // the list changed under the focus (a save was deleted)

    // A confirmation over the list: the question, with Cross to confirm and Circle to go back.
    enum class Prompt : u8 { None, Overwrite, Delete, Duplicate, Failed };
    const char* failure_title{""};  // what the Failed prompt says
    std::string failure;
    enum class Answer : u8 { None, Confirm, Back };
    Prompt prompt{Prompt::None};
    int prompt_idx{0};
    int prompt_frame{0};

public:
    explicit SaveDialogUi(SaveDialogState* state = nullptr, CommonDialog::Status* status = nullptr,
                          SaveDialogResult* result = nullptr);

    ~SaveDialogUi() override;
    SaveDialogUi(const SaveDialogUi& other) = delete;
    SaveDialogUi(SaveDialogUi&& other) noexcept;
    SaveDialogUi& operator=(SaveDialogUi& other) = delete;
    SaveDialogUi& operator=(SaveDialogUi&& other) noexcept;

    void Open(SaveDialogState* state, CommonDialog::Status* status, SaveDialogResult* result);
    void Reset(bool clear_load_list = false);
    void Finish(ButtonId buttonId, CommonDialog::Result r = CommonDialog::Result::OK);

    [[nodiscard]] std::recursive_mutex& GetMutex() {
        return draw_mutex;
    }

    void Draw() override;

private:
    void DrawItem(int id, const SaveDialogState::Item& item, bool clickable, bool pin_focus = false);

    void DrawList(bool interactive = true);
    Answer DrawNotice(const ImGuiIO& io, const char* title, const std::string& message, bool confirm, bool take_input, bool busy = false);
    void DrawLoadFailed(const ImGuiIO& io);
    void DrawPrompt(const ImGuiIO& io);
    void OpenPrompt(Prompt kind, int index);
    bool CancelModal();
    void DrawUser();
    void DrawSystemMessage();
    void DrawErrorCode();
    void DrawProgressBar();
    void StartLinger(const char* title, std::string message);
    void EndLinger();
    void DrawLingering(const ImGuiIO& io);
};

}; // namespace Libraries::SaveData::Dialog
