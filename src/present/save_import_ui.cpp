// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/save_import_ui.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <stdexcept>
#include "main/host.h"
#include "main/save_import.h"
#include "orbis/common/path_util.h"
#include "present/prompt.h"
#include "ui/imgui_core.h"

namespace Gow3::SaveImportUi {
namespace {
using namespace std::chrono_literals;
using SaveImport::Source;
using Prompt::Answer;
enum class Page { Closed, Source, Searching, Browse, Picking, Confirm, Existing, Copying, Success, Error };

Page page{};
SDL_Window* window{};
std::filesystem::path game;
std::filesystem::path browse_start;
Source source{};
SaveImport::SaveSet candidate;
std::future<std::optional<SaveImport::SaveSet>> search;
std::future<std::filesystem::path> picked;
std::future<size_t> copy;
std::chrono::steady_clock::time_point restart_at;
constexpr auto title = "IMPORT SAVES";

void Show(Page next, Prompt::Content content) {
    page = next;
    content.step_layout = true;
    Prompt::Show(std::move(content));
}

void Close() {
    page = Page::Closed;
    Prompt::Hide();
    ImGui::Core::ReleaseGamepadInputCapture();
    candidate = {};
}

void Decline() {
    try { SaveImport::MarkOffered(); }
    catch (const std::exception& error) { Log(error.what()); }
    Close();
}

void ChooseSource() {
    Show(Page::Source, {.title = title, .question = "WHERE DO YOU WANT TO IMPORT FROM?",
         .actions = {"SHADPS4", "SPARTANPORT", "SKIP"}});
}

void Search(std::filesystem::path folder = {}) {
    search = std::async(std::launch::async, SaveImport::Discover, source, game, std::move(folder));
    Show(Page::Searching, {.title = title, .question = "LOOKING FOR SAVES...", .status = true});
}

void Browse(const char* reason) {
    Show(Page::Browse, {.title = title, .question = reason, .notes = {"You can choose the installation or save folder."},
         .actions = {"CHOOSE FOLDER", "SKIP", "BACK"}});
}

void PickFolder() {
    auto result = std::make_unique<std::promise<std::filesystem::path>>();
    picked = result->get_future();
    Show(Page::Picking, {.title = title, .question = "CHOOSE A SAVE FOLDER", .status = true});
    const auto location = Common::FS::PathToUTF8String(browse_start.empty() ? game : browse_start);
    SDL_ShowOpenFolderDialog([](void* data, const char* const* files, int) {
        const std::unique_ptr<std::promise<std::filesystem::path>> promise(
            static_cast<std::promise<std::filesystem::path>*>(data));
        try {
            if (!files) throw std::runtime_error(SDL_GetError());
            promise->set_value(*files ? std::filesystem::path(reinterpret_cast<const char8_t*>(*files)) : std::filesystem::path{});
        } catch (...) { promise->set_exception(std::current_exception()); }
    }, result.release(), window, location.c_str(), false);
}

std::string Count(size_t count) {
    return std::format("{} save{}", count, count == 1 ? "" : "s");
}

std::string Summary(const SaveImport::SaveSet& set) {
    const auto recent = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::seconds>(set.slots.front().time - std::chrono::file_clock::now());
    const auto time = std::chrono::system_clock::to_time_t(recent);
    std::tm local{};
    if (localtime_s(&local, &time)) return Count(set.slots.size()) + " found";
    char date[64];
    if (!std::strftime(date, sizeof(date), "%d %b %Y, %H:%M", &local)) return Count(set.slots.size()) + " found";
    return std::format("{} found. Most recent: {}.", Count(set.slots.size()), date);
}

void Confirm() {
    const auto& set = candidate;
    if (set.conflicts == set.slots.size()) {
        Show(Page::Existing, {.title = title, .question = "ALL FOUND SAVES ALREADY EXIST.",
             .notes = {Summary(set)}, .actions = {"DONE", "", "BACK"}});
        return;
    }
    Show(Page::Confirm, {title, "DO YOU WANT TO IMPORT THESE SAVES?", {Summary(set),
         set.conflicts ? std::format("{} already present. {} will be imported.",
                                     Count(set.conflicts), Count(set.slots.size() - set.conflicts)) : ""},
         true});
}

template<class T>
bool Ready(const std::future<T>& task) { return task.wait_for(0s) == std::future_status::ready; }
} // namespace

void Offer(SDL_Window* parent, const std::filesystem::path& game_directory) {
    if (SaveImport::WasOffered()) return;
    window = parent;
    game = game_directory;
    ImGui::Core::AcquireGamepadInputCapture();
    ChooseSource();
}

bool Active() { return page != Page::Closed; }

bool Update() {
    if (!Active()) return false;
    const auto answer = Prompt::Result();
    if (page == Page::Success) return std::chrono::steady_clock::now() >= restart_at;
    try {
        switch (page) {
        case Page::Source:
            if (answer == Answer::Back) Decline();
            if (answer == Answer::Yes || answer == Answer::No) {
                source = answer == Answer::Yes ? Source::ShadPS4 : Source::SpartanPort;
                browse_start.clear();
                Search();
            }
            break;
        case Page::Searching: {
            if (!Ready(search)) break;
            auto found = search.get();
            if (!found) {
                Browse("NO COMPATIBLE SAVES FOUND.");
            } else {
                candidate = std::move(*found);
                Confirm();
            }
            break;
        }
        case Page::Browse:
            if (answer == Answer::Yes) PickFolder();
            if (answer == Answer::No) Decline();
            if (answer == Answer::Back) ChooseSource();
            break;
        case Page::Picking:
            if (Ready(picked)) {
                auto path = picked.get();
                if (path.empty()) {
                    Browse("CHOOSE A SAVE FOLDER.");
                } else {
                    browse_start = path;
                    Search(std::move(path));
                }
            }
            break;
        case Page::Confirm:
            if (answer == Answer::No) Decline();
            if (answer == Answer::Yes) {
                copy = std::async(std::launch::async, SaveImport::Import, source, std::move(candidate));
                Show(Page::Copying, {.title = title, .question = "IMPORTING SAVES...", .status = true});
            }
            break;
        case Page::Existing:
            if (answer == Answer::Yes) Decline();
            if (answer == Answer::Back) ChooseSource();
            break;
        case Page::Copying:
            if (Ready(copy)) {
                Show(Page::Success, {.title = "SAVES IMPORTED", .question = Count(copy.get()) + " imported successfully.",
                     .notes = {"Restarting the game..."}, .status = true});
                restart_at = std::chrono::steady_clock::now() + 2s;
            }
            break;
        case Page::Error:
            if (answer == Answer::Yes) ChooseSource();
            if (answer == Answer::No || answer == Answer::Back) Close();
            break;
        default:
            break;
        }
    } catch (const std::exception& error) {
        Log(std::string("Save import: ") + error.what());
        Show(Page::Error, {.title = title, .question = "IMPORT COULD NOT BE COMPLETED.", .notes = {error.what()},
             .actions = {"TRY AGAIN", "", "BACK"}});
    }
    return false;
}

void WaitForCopy() {
    if (copy.valid()) copy.wait();
}

void RestartFailed() { Close(); }
} // namespace Gow3::SaveImportUi
