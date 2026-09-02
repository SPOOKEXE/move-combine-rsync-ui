#include "dir_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

void refresh(DirBrowser& browser) {
    browser.directories.clear();
    browser.error.clear();
    std::error_code ec;
    fs::directory_iterator iterator(browser.cwd, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        browser.error = ec.message();
        return;
    }
    for (const auto& entry : iterator) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] != '.') browser.directories.push_back(name);
    }
    std::sort(browser.directories.begin(), browser.directories.end());
    std::snprintf(browser.path, sizeof(browser.path), "%s", browser.cwd.c_str());
}

void enter(DirBrowser& browser, const fs::path& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        browser.error = "not a readable folder";
        return;
    }
    browser.cwd = fs::weakly_canonical(path, ec).string();
    if (ec) browser.cwd = path.lexically_normal().string();
    refresh(browser);
}

}  // namespace

void openDirBrowser(DirBrowser& browser, const char* title, const std::string& start) {
    browser.title = title;
    const char* home = std::getenv("HOME");
    fs::path initial = start.empty() ? fs::path(home ? home : "/") : fs::path(start);
    std::error_code ec;
    if (!fs::is_directory(initial, ec)) initial = initial.parent_path();
    if (initial.empty() || !fs::is_directory(initial, ec)) initial = "/";
    enter(browser, initial);
    browser.open = true;
    browser.openPopup = true;
}

bool drawDirBrowser(DirBrowser& browser, std::string& selected) {
    if (!browser.open) return false;
    if (browser.openPopup) {
        ImGui::OpenPopup(browser.title.c_str());
        browser.openPopup = false;
    }

    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(browser.title.c_str(), &browser.open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }

    ImGui::SetNextItemWidth(-95.0f);
    const bool submit = ImGui::InputText("##path", browser.path, sizeof(browser.path),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(85, 0)) || submit) enter(browser, browser.path);

    if (ImGui::Button("Up", ImVec2(85, 0))) enter(browser, fs::path(browser.cwd).parent_path());
    ImGui::SameLine();
    ImGui::TextUnformatted(browser.cwd.c_str());
    if (!browser.error.empty()) ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "%s", browser.error.c_str());

    ImGui::BeginChild("folders", ImVec2(0, -48), true);
    for (const auto& name : browser.directories) {
        if (ImGui::Selectable((name + "/").c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            enter(browser, fs::path(browser.cwd) / name);
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("Choose this folder", ImVec2(180, 0))) {
        selected = browser.cwd;
        browser.open = false;
        ImGui::CloseCurrentPopup();
        picked = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        browser.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return picked;
}
