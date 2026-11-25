// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/save_import_ui.h"
#include "ui/translation.h"
#include <fmt/format.h>

#include <SDL3/SDL.h>
#include <chrono>
#include <ctime>
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
bool manual_import{};
SDL_Window* window{};
std::filesystem::path game;
std::filesystem::path browse_start;
Source source{};
SaveImport::SaveSet candidate;
std::future<std::optional<SaveImport::SaveSet>> search;
std::future<std::filesystem::path> picked;
std::future<size_t> copy;
std::chrono::steady_clock::time_point restart_at;
using UI::Tr;
const char* Title() { return Tr("Import saves"); }

void Show(Page next, Prompt::Content content) {
    page = next;
    content.step_layout = true;
    content.navigate = manual_import;
    Prompt::Show(std::move(content));
}

void Close() {
    page = Page::Closed;
    Prompt::Hide();
    ImGui::Core::ReleaseGamepadInputCapture();
    candidate = {};
}

void Decline() {
    if (!manual_import) {
        try { SaveImport::MarkOffered(); }
        catch (const std::exception& error) { Log(error.what()); }
    }
    Close();
}

void ChooseSource() {
    Show(Page::Source, {.title = Title(), .question = Tr("Where do you want to import from?"),
         .actions = {"shadPS4", "SpartanPort", Tr(manual_import ? "Cancel" : "Skip")}});
}

void Search(std::filesystem::path folder = {}) {
    search = std::async(std::launch::async, SaveImport::Discover, source, game, std::move(folder));
    Show(Page::Searching, {.title = Title(), .question = Tr("Looking for saves…"), .status = true});
}

void Browse(const char* reason) {
    Show(Page::Browse, {.title = Title(), .question = reason, .notes = {Tr("You can choose the installation or save folder.")},
         .actions = {Tr("Choose folder"), Tr(manual_import ? "Cancel" : "Skip"), Tr("Back")}});
}

void PickFolder() {
    auto result = std::make_unique<std::promise<std::filesystem::path>>();
    picked = result->get_future();
    Show(Page::Picking, {.title = Title(), .question = Tr("Choose a save folder"), .status = true});
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

std::string Summary(const SaveImport::SaveSet& set) {
    auto result = fmt::format(fmt::runtime(Tr("Saves found: {}")), set.slots.size());
    const auto recent = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::seconds>(set.slots.front().time - std::chrono::file_clock::now());
    const auto time = std::chrono::system_clock::to_time_t(recent);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time)) return result;
#else
    if (!localtime_r(&time, &local)) return result;
#endif
    char date[64];
    if (std::strftime(date, sizeof(date), "%Y-%m-%d · %H:%M", &local))
        result += fmt::format(fmt::runtime(Tr(" · Most recent: {}")), date);
    return result;
}

void Confirm() {
    const auto& set = candidate;
    if (set.conflicts == set.slots.size()) {
        Show(Page::Existing, {.title = Title(), .question = Tr("All found saves already exist."),
             .notes = {Summary(set)}, .actions = {Tr("Done"), "", Tr("Back")}});
        return;
    }
    std::string note;
    if (set.conflicts)
        note = fmt::format(fmt::runtime(Tr("Already present: {} · To import: {}")), set.conflicts, set.slots.size() - set.conflicts);
    if (set.archive_autosave) {
        if (!note.empty()) note += '\n';
        note += Tr("The current autosave will be kept as a separate save.");
    }
    Show(Page::Confirm, {Title(), Tr("Do you want to import these saves?"), {Summary(set), std::move(note)}, true});
}

template<class T>
bool Ready(const std::future<T>& task) { return task.wait_for(0s) == std::future_status::ready; }
} // namespace

void Offer(SDL_Window* parent, const std::filesystem::path& game_directory, bool manual) {
    if (Active() || (!manual && SaveImport::WasOffered())) return;
    manual_import = manual;
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
                Browse(Tr("No compatible saves found."));
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
                    Browse(Tr("Choose a save folder"));
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
                Show(Page::Copying, {.title = Title(), .question = Tr("Importing saves…"), .status = true});
            }
            break;
        case Page::Existing:
            if (answer == Answer::Yes) Decline();
            if (answer == Answer::Back) ChooseSource();
            break;
        case Page::Copying:
            if (Ready(copy)) {
                Show(Page::Success, {.title = Tr("Saves imported"), .question = fmt::format(fmt::runtime(Tr("Saves imported: {}")), copy.get()),
                     .notes = {Tr("Restarting…")}, .status = true});
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
        Show(Page::Error, {.title = Title(), .question = Tr("Import could not be completed."), .notes = {Tr(error.what())},
             .actions = {Tr("Try again"), "", Tr("Back")}});
    }
    return false;
}

void WaitForCopy() {
    if (copy.valid()) copy.wait();
}

void RestartFailed() { Close(); }
} // namespace Gow3::SaveImportUi
