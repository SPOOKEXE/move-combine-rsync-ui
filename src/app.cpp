#include "app.h"

#include <signal.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "rsync_process.h"

namespace fs = std::filesystem;

namespace {

constexpr ImVec4 kDim(0.52f, 0.55f, 0.60f, 1.0f);
constexpr ImVec4 kGood(0.30f, 0.86f, 0.55f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.70f, 0.22f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.34f, 0.30f, 1.0f);

void addLog(WorkerState& worker, const std::string& line) {
    worker.log.push_back(line);
    if (worker.log.size() > 1000) worker.log.erase(worker.log.begin());
}

void setFailure(AppState& state, const std::string& message) {
    std::lock_guard lock(state.mutex);
    state.worker.phase = AppPhase::Failed;
    state.worker.status = message;
    addLog(state.worker, message);
}

void processStarted(AppState& state, pid_t pid) {
    std::lock_guard lock(state.mutex);
    state.worker.pid = pid;
    if (state.worker.cancelRequested) kill(-pid, SIGTERM);
}

bool processFinished(AppState& state) {
    std::lock_guard lock(state.mutex);
    state.worker.pid = 0;
    return state.worker.cancelRequested;
}

bool cancellationRequested(AppState& state) {
    std::lock_guard lock(state.mutex);
    return state.worker.cancelRequested;
}

void cancelWork(AppState& state) {
    std::lock_guard lock(state.mutex);
    if (state.worker.phase != AppPhase::Scanning && state.worker.phase != AppPhase::Merging) return;
    state.worker.cancelRequested = true;
    state.worker.status = "Stopping rsync";
    if (state.worker.pid > 0) kill(-state.worker.pid, SIGTERM);
}

void startScan(AppState& state) {
    if (state.thread.joinable()) state.thread.join();
    const std::vector<std::string> sources = state.sources;
    const std::string destination = state.destination;
    {
        std::lock_guard lock(state.mutex);
        state.worker = {};
        state.worker.phase = AppPhase::Scanning;
        state.worker.status = "Checking folders";
    }

    state.thread = std::thread([&state, sources, destination] {
        const std::string invalid = validateInputs(sources, destination);
        if (!invalid.empty()) {
            setFailure(state, invalid);
            return;
        }

        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto args = buildRsyncArgs(sources[i], destination, {}, RsyncMode::DryRun);
            {
                std::lock_guard lock(state.mutex);
                state.worker.currentSource = static_cast<int>(i);
                state.worker.status = "Dry run " + std::to_string(i + 1) + " of " +
                                      std::to_string(sources.size());
                addLog(state.worker, "$ " + displayCommand(args));
            }
            const int code = runRsync(args, {
                .onProgress = [&state, i, total = sources.size()](const RsyncProgress& progress) {
                    std::lock_guard lock(state.mutex);
                    state.worker.progress = (static_cast<float>(i) + progress.fraction) /
                                            static_cast<float>(total);
                },
                .onLine = [&state](const std::string& line) {
                    // Itemized changes are useful evidence but too noisy for the main view.
                    if (line.size() > 11 && (line[0] == '>' || line[0] == '.' || line[0] == 'c')) return;
                    std::lock_guard lock(state.mutex);
                    addLog(state.worker, line);
                },
                .onStarted = [&state](pid_t pid) { processStarted(state, pid); },
            });
            const bool cancelled = processFinished(state);
            if (code != 0 || cancelled) {
                setFailure(state, cancelled ? "Dry run cancelled"
                                 : code == 127 ? "rsync was not found on PATH"
                                              : "dry run failed with exit code " + std::to_string(code));
                return;
            }
        }

        std::string error;
        MergePlan plan = inspectMerge(sources, destination, error,
                                      [&state] { return cancellationRequested(state); });
        if (!error.empty()) {
            setFailure(state, cancellationRequested(state) ? "Dry run cancelled" : error);
            return;
        }
        std::lock_guard lock(state.mutex);
        state.worker.plan = std::move(plan);
        state.worker.progress = 1.0f;
        if (state.worker.plan.conflicts.empty()) {
            state.worker.phase = AppPhase::Ready;
            state.worker.status = "Dry run complete. No conflicts.";
        } else {
            state.worker.phase = AppPhase::Resolve;
            state.worker.status = std::to_string(state.worker.plan.conflicts.size()) +
                                  " conflicts ready; collision rename is the default";
        }
    });
}

void startMerge(AppState& state) {
    if (state.thread.joinable()) state.thread.join();
    MergePlan plan;
    {
        std::lock_guard lock(state.mutex);
        std::string error;
        if (!prepareMerge(state.worker.plan, error)) {
            state.worker.status = error;
            return;
        }
        plan = state.worker.plan;
        state.worker.phase = AppPhase::Merging;
        state.worker.progress = 0.0f;
        state.worker.status = "Merging folders";
    }

    state.thread = std::thread([&state, plan = std::move(plan)]() mutable {
        for (const auto& relative : plan.destinationRemovals) {
            if (cancellationRequested(state)) {
                setFailure(state, "Merge cancelled");
                return;
            }
            std::error_code ec;
            const fs::path target = fs::path(plan.destination) / fs::path(relative);
            fs::remove_all(target, ec);
            if (ec) {
                setFailure(state, "could not replace " + relative + ": " + ec.message());
                return;
            }
        }

        const std::size_t totalSteps = plan.sources.size() + plan.renamedCopies.size();
        for (std::size_t i = 0; i < plan.sources.size(); ++i) {
            const std::string exclude = writeExcludeFile(plan.exclusions[i]);
            if (!plan.exclusions[i].empty() && exclude.empty()) {
                setFailure(state, "could not create the temporary conflict filter");
                return;
            }
            const auto args = buildRsyncArgs(plan.sources[i], plan.destination, exclude,
                                             RsyncMode::Merge);
            {
                std::lock_guard lock(state.mutex);
                state.worker.currentSource = static_cast<int>(i);
                state.worker.status = "Merging " + std::to_string(i + 1) + " of " +
                                      std::to_string(plan.sources.size());
                addLog(state.worker, "$ " + displayCommand(args));
            }
            const int code = runRsync(args, {
                .onProgress = [&state, i, totalSteps](const RsyncProgress& progress) {
                    std::lock_guard lock(state.mutex);
                    state.worker.progress = (static_cast<float>(i) + progress.fraction) /
                                            static_cast<float>(totalSteps);
                    state.worker.status = progress.transferred + "  " + progress.speed +
                                          "  " + progress.eta;
                },
                .onLine = [&state](const std::string& line) {
                    std::lock_guard lock(state.mutex);
                    addLog(state.worker, line);
                },
                .onStarted = [&state](pid_t pid) { processStarted(state, pid); },
            });
            const bool cancelled = processFinished(state);
            if (!exclude.empty()) std::remove(exclude.c_str());
            if (code != 0 || cancelled) {
                setFailure(state, cancelled ? "Merge cancelled"
                                            : "merge failed with exit code " + std::to_string(code));
                return;
            }
        }

        for (std::size_t i = 0; i < plan.renamedCopies.size(); ++i) {
            if (cancellationRequested(state)) {
                setFailure(state, "Merge cancelled");
                return;
            }
            const RenamedCopy& copy = plan.renamedCopies[i];
            const fs::path source = fs::path(plan.sources[copy.source]) / copy.fromRelative;
            const fs::path destination = fs::path(plan.destination) / copy.toRelative;
            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            if (ec) {
                setFailure(state, "could not create folder for " + copy.toRelative + ": " +
                                      ec.message());
                return;
            }
            const fs::file_status destinationStatus = fs::symlink_status(destination, ec);
            if (!ec && destinationStatus.type() != fs::file_type::not_found) {
                setFailure(state, "collision name became occupied: " + copy.toRelative);
                return;
            }

            const auto args = buildRsyncCopyArgs(source.string(), destination.string(),
                                                 copy.kind == EntryKind::Directory,
                                                 RsyncMode::Merge);
            const std::size_t step = plan.sources.size() + i;
            {
                std::lock_guard lock(state.mutex);
                state.worker.currentSource = copy.source;
                state.worker.status = "Saving collision as " + copy.toRelative;
                addLog(state.worker, "$ " + displayCommand(args));
            }
            const int code = runRsync(args, {
                .onProgress = [&state, step, totalSteps](const RsyncProgress& progress) {
                    std::lock_guard lock(state.mutex);
                    state.worker.progress = (static_cast<float>(step) + progress.fraction) /
                                            static_cast<float>(totalSteps);
                },
                .onLine = [&state](const std::string& line) {
                    std::lock_guard lock(state.mutex);
                    addLog(state.worker, line);
                },
                .onStarted = [&state](pid_t pid) { processStarted(state, pid); },
            });
            const bool cancelled = processFinished(state);
            if (code != 0 || cancelled) {
                setFailure(state, cancelled ? "Merge cancelled"
                                            : "renamed copy failed with exit code " +
                                                  std::to_string(code));
                return;
            }
        }

        std::lock_guard lock(state.mutex);
        state.worker.phase = AppPhase::Done;
        state.worker.progress = 1.0f;
        state.worker.status = "Merge complete";
        addLog(state.worker, "merge complete");
    });
}

void reset(AppState& state) {
    if (state.thread.joinable()) state.thread.join();
    std::lock_guard lock(state.mutex);
    state.worker = {};
}

std::string candidateLabel(const Candidate& candidate) {
    std::string label = candidate.source < 0
        ? "Keep destination"
        : "Use source " + std::to_string(candidate.source + 1);
    label += "  |  ";
    label += entryKindName(candidate.kind);
    if (candidate.kind == EntryKind::File) label += "  " + formatSize(candidate.size);
    label += "  " + formatTime(candidate.modified);
    return label;
}

void drawSetup(AppState& state) {
    ImGui::TextUnformatted("SOURCE FOLDERS");
    ImGui::TextColored(kDim, "Contents are layered in this order. Later sources only win when you choose them.");
    ImGui::BeginChild("sources", ImVec2(0, 190), true);
    int remove = -1;
    int moveUp = -1;
    int moveDown = -1;
    for (std::size_t i = 0; i < state.sources.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("up")) moveUp = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == state.sources.size());
        if (ImGui::SmallButton("down")) moveDown = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%zu", i + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(state.sources[i].c_str());
        ImGui::PopID();
    }
    if (state.sources.empty()) ImGui::TextColored(kDim, "No source folders yet");
    ImGui::EndChild();
    if (moveUp > 0) std::swap(state.sources[moveUp], state.sources[moveUp - 1]);
    if (moveDown >= 0 && moveDown + 1 < static_cast<int>(state.sources.size())) {
        std::swap(state.sources[moveDown], state.sources[moveDown + 1]);
    }
    if (remove >= 0) state.sources.erase(state.sources.begin() + remove);

    ImGui::SetNextItemWidth(-190);
    bool add = ImGui::InputTextWithHint("##source", "/path/to/source", state.sourceInput,
                                        sizeof(state.sourceInput),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(75, 0))) add = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(95, 0))) {
        state.browserPurpose = BrowserPurpose::AddSource;
        openDirBrowser(state.browser, "Add source folder", state.sourceInput);
    }
    if (add && state.sourceInput[0] != '\0') {
        state.sources.emplace_back(state.sourceInput);
        state.sourceInput[0] = '\0';
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("DESTINATION");
    ImGui::SetNextItemWidth(-115);
    ImGui::InputTextWithHint("##destination", "/path/to/combined-folder",
                             state.destinationInput, sizeof(state.destinationInput));
    state.destination = state.destinationInput;
    ImGui::SameLine();
    if (ImGui::Button("Browse##destination", ImVec2(105, 0))) {
        state.browserPurpose = BrowserPurpose::Destination;
        openDirBrowser(state.browser, "Choose destination", state.destination);
    }

    ImGui::Spacing();
    const std::string invalid = validateInputs(state.sources, state.destination);
    ImGui::BeginDisabled(!invalid.empty());
    if (ImGui::Button("Dry run", ImVec2(150, 36))) startScan(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(invalid.empty() ? kDim : kBad, "%s",
                       invalid.empty() ? "Nothing is written until the review is complete."
                                       : invalid.c_str());
}

void selectQuick(MergePlan& plan, int source) {
    for (auto& conflict : plan.conflicts) {
        conflict.selected = -1;
        for (std::size_t i = 0; i < conflict.candidates.size(); ++i) {
            if (conflict.candidates[i].source == source) conflict.selected = static_cast<int>(i);
        }
    }
}

void selectRename(MergePlan& plan) {
    for (auto& conflict : plan.conflicts) conflict.selected = kRenameCollisions;
}

void drawReview(AppState& state) {
    MergePlan& plan = state.worker.plan;
    const int unresolved = static_cast<int>(std::count_if(
        plan.conflicts.begin(), plan.conflicts.end(), [](const Conflict& conflict) {
            return conflict.selected != kRenameCollisions &&
                   (conflict.selected < 0 ||
                    conflict.selected >= static_cast<int>(conflict.candidates.size()));
        }));

    ImGui::Text("%zu conflicts", plan.conflicts.size());
    ImGui::SameLine();
    ImGui::TextColored(unresolved == 0 ? kGood : kWarn, "%d unresolved", unresolved);
    if (!plan.conflicts.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename all collisions")) selectRename(plan);
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep destination copies")) selectQuick(plan, -1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use latest source")) {
            for (auto& conflict : plan.conflicts) {
                int best = -1;
                int bestSource = -1;
                for (std::size_t i = 0; i < conflict.candidates.size(); ++i) {
                    if (conflict.candidates[i].source > bestSource) {
                        bestSource = conflict.candidates[i].source;
                        best = static_cast<int>(i);
                    }
                }
                conflict.selected = best;
            }
        }
    }

    if (ImGui::BeginTable("conflicts", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -72))) {
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("keep", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (std::size_t row = 0; row < plan.conflicts.size(); ++row) {
            Conflict& conflict = plan.conflicts[row];
            ImGui::PushID(static_cast<int>(row));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(conflict.relativePath.c_str());
            ImGui::TableSetColumnIndex(1);
            const bool validCandidate = conflict.selected >= 0 &&
                conflict.selected < static_cast<int>(conflict.candidates.size());
            const char* preview = conflict.selected == kRenameCollisions
                ? "Rename collisions (keep every copy)"
                : validCandidate ? nullptr : "Choose a copy...";
            const std::string selectedLabel = validCandidate
                ? candidateLabel(conflict.candidates[conflict.selected])
                : std::string{};
            if (validCandidate) preview = selectedLabel.c_str();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##winner", preview)) {
                if (ImGui::Selectable("Rename collisions (keep every copy)",
                                      conflict.selected == kRenameCollisions)) {
                    conflict.selected = kRenameCollisions;
                }
                for (std::size_t i = 0; i < conflict.candidates.size(); ++i) {
                    const std::string label = candidateLabel(conflict.candidates[i]);
                    if (ImGui::Selectable(label.c_str(), conflict.selected == static_cast<int>(i))) {
                        conflict.selected = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(unresolved != 0);
    if (ImGui::Button("Merge now", ImVec2(150, 36))) startMerge(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(90, 36))) reset(state);
}

}  // namespace

void applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(20, 18);
    style.FramePadding = ImVec2(9, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.FrameRounding = 2;
    style.PopupRounding = 2;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.025f, 0.025f, 0.025f, 1);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.025f, 0.025f, 0.025f, 1);
    style.Colors[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_TextDisabled] = kDim;
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.09f, 0.09f, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.15f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.13f, 0.13f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.16f, 0.16f, 1);
    style.Colors[ImGuiCol_CheckMark] = kGood;
    style.Colors[ImGuiCol_PlotHistogram] = kGood;
}

void drawApp(AppState& state) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Merge folders", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("MERGE FOLDERS");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "combine many folder trees into one with rsync");
    ImGui::Separator();

    WorkerState snapshot;
    {
        std::lock_guard lock(state.mutex);
        snapshot = state.worker;
    }
    if (snapshot.phase == AppPhase::Setup) {
        drawSetup(state);
    } else if (snapshot.phase == AppPhase::Resolve || snapshot.phase == AppPhase::Ready) {
        // These phases are published only after the scan thread has stopped
        // touching state, so join it before the UI edits conflict choices.
        if (state.thread.joinable()) state.thread.join();
        if (snapshot.phase == AppPhase::Ready) {
            ImGui::TextColored(kGood, "%s", snapshot.status.c_str());
            if (ImGui::Button("Merge now", ImVec2(150, 36))) startMerge(state);
            ImGui::SameLine();
            if (ImGui::Button("Back", ImVec2(90, 36))) reset(state);
        } else {
            drawReview(state);
        }
    } else {
        const ImVec4 color = snapshot.phase == AppPhase::Failed ? kBad
                             : snapshot.phase == AppPhase::Done ? kGood : ImVec4(1, 1, 1, 1);
        ImGui::TextColored(color, "%s", snapshot.status.c_str());
        if (snapshot.phase == AppPhase::Scanning || snapshot.phase == AppPhase::Merging) {
            ImGui::ProgressBar(snapshot.progress, ImVec2(-1, 28));
            ImGui::TextColored(kDim, "source %d of %zu", snapshot.currentSource + 1,
                               state.sources.size());
            if (ImGui::Button(snapshot.cancelRequested ? "Stopping..." : "Cancel",
                              ImVec2(100, 32)) && !snapshot.cancelRequested) {
                cancelWork(state);
            }
        } else {
            if (ImGui::Button("Start another merge", ImVec2(180, 36))) reset(state);
        }
    }

    if (snapshot.phase != AppPhase::Setup) {
        ImGui::Separator();
        ImGui::Checkbox("Show log", &state.showLog);
        if (state.showLog) {
            ImGui::BeginChild("log", ImVec2(0, 180), true);
            for (const auto& line : snapshot.log) ImGui::TextUnformatted(line.c_str());
            ImGui::EndChild();
        }
    }
    ImGui::End();

    std::string selected;
    if (drawDirBrowser(state.browser, selected)) {
        if (state.browserPurpose == BrowserPurpose::AddSource) {
            state.sources.push_back(selected);
            state.sourceInput[0] = '\0';
        } else if (state.browserPurpose == BrowserPurpose::Destination) {
            state.destination = selected;
            std::snprintf(state.destinationInput, sizeof(state.destinationInput), "%s",
                          selected.c_str());
        }
        state.browserPurpose = BrowserPurpose::None;
    }
}

void shutdownApp(AppState& state) {
    cancelWork(state);
    if (state.thread.joinable()) state.thread.join();
}
