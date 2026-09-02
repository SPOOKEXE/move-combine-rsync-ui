#include "merge_plan.h"
#include "rsync_process.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void write(const fs::path& path, const char* text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << text;
}

const Conflict* findConflict(const MergePlan& plan, const std::string& path) {
    for (const auto& conflict : plan.conflicts) {
        if (conflict.relativePath == path) return &conflict;
    }
    return nullptr;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "merge-folders-tests";
    fs::remove_all(root);
    const fs::path a = root / "a";
    const fs::path b = root / "b";
    const fs::path out = root / "out";
    fs::create_directories(a / "shared-dir");
    fs::create_directories(b / "shared-dir");
    fs::create_directories(out);
    write(a / "only-a.txt", "a");
    write(a / "same.txt", "first");
    write(b / "same.txt", "second");
    write(out / "same.txt", "old");
    write(a / "shape", "file");
    write(b / "shape" / "child.txt", "child");
    write(out / "shape", "old shape");
    const std::string oddName = "odd\nname[1]*.txt";
    write(a / oddName, "odd-first");
    write(b / oddName, "odd-second");
    write(a / "shared-dir" / "a.txt", "a");
    write(b / "shared-dir" / "b.txt", "b");

    std::string error;
    MergePlan plan = inspectMerge({a.string(), b.string()}, out.string(), error);
    check(error.empty(), "valid folders inspect cleanly");
    check(findConflict(plan, "same.txt") != nullptr, "overlapping files conflict");
    check(findConflict(plan, "shape") != nullptr, "file and folder shape conflict");
    check(findConflict(plan, oddName) != nullptr, "newline and wildcard filename conflicts");
    check(findConflict(plan, "shared-dir") == nullptr, "folders merge without conflict");
    check(allConflictsResolved(plan), "collision rename resolves conflicts by default");
    check(std::all_of(plan.conflicts.begin(), plan.conflicts.end(), [](const Conflict& conflict) {
              return conflict.selected == kRenameCollisions;
          }),
          "every conflict defaults to collision rename");

    for (auto& conflict : plan.conflicts) {
        for (std::size_t i = 0; i < conflict.candidates.size(); ++i) {
            if (conflict.candidates[i].source == 1) conflict.selected = static_cast<int>(i);
        }
    }
    check(allConflictsResolved(plan), "choices resolve every conflict");
    check(prepareMerge(plan, error), "resolved plan prepares");
    check(std::find(plan.exclusions[0].begin(), plan.exclusions[0].end(), "same.txt") !=
              plan.exclusions[0].end(),
          "losing source path is excluded");
    check(std::find(plan.destinationRemovals.begin(), plan.destinationRemovals.end(), "shape") !=
              plan.destinationRemovals.end(),
          "destination shape mismatch is removed when present");

    const auto dry = buildRsyncArgs(a.string(), out.string(), {}, RsyncMode::DryRun);
    check(std::find(dry.begin(), dry.end(), "--dry-run") != dry.end(),
          "dry run argv cannot write");
    check(dry[dry.size() - 2].back() == '/', "source contents use a trailing slash");

    check(runRsync(dry, {}) == 0, "real rsync dry run succeeds");
    check(!fs::exists(out / "only-a.txt"), "dry run writes nothing");

    for (const auto& relative : plan.destinationRemovals) fs::remove_all(out / relative);
    for (std::size_t i = 0; i < plan.sources.size(); ++i) {
        const std::string exclude = writeExcludeFile(plan.exclusions[i]);
        check(plan.exclusions[i].empty() || !exclude.empty(), "exclude file is created");
        const auto args = buildRsyncArgs(plan.sources[i], plan.destination, exclude,
                                         RsyncMode::Merge);
        check(runRsync(args, {}) == 0, "real rsync merge succeeds");
        if (!exclude.empty()) std::remove(exclude.c_str());
    }
    check(fs::exists(out / "only-a.txt"), "unique files are merged");
    check(fs::is_directory(out / "shape"), "chosen folder replaces destination file");
    check(fs::exists(out / "shape" / "child.txt"), "chosen folder contents are copied");
    {
        std::ifstream merged(out / oddName);
        std::string contents;
        merged >> contents;
        check(contents == "odd-second", "null-delimited filter handles hostile filenames");
    }
    {
        std::ifstream merged(out / "same.txt");
        std::string contents;
        merged >> contents;
        check(contents == "second", "chosen later source wins file conflict");
    }

    check(!validateInputs({a.string()}, (a / "nested").string()).empty(),
          "nested destination is rejected");
    const fs::path newOutput = root / "new-output";
    check(validateInputs({a.string()}, newOutput.string()).empty(),
          "a new destination below an existing parent is allowed");
    MergePlan newPlan = inspectMerge({a.string()}, newOutput.string(), error);
    check(error.empty() && newPlan.conflicts.empty(), "new destination begins without conflicts");

    const fs::path renameA = root / "rename-a";
    const fs::path renameB = root / "rename-b";
    const fs::path renameOut = root / "rename-out";
    fs::create_directories(renameA);
    fs::create_directories(renameB);
    fs::create_directories(renameOut);
    write(renameA / "report.txt", "first-copy");
    write(renameB / "report.txt", "second-copy");
    write(renameOut / "report.txt", "destination-copy");
    write(renameOut / "report_collision_0001.txt", "already-here");

    MergePlan renamePlan = inspectMerge(
        {renameA.string(), renameB.string()}, renameOut.string(), error);
    check(error.empty(), "rename plan inspects cleanly");
    check(prepareMerge(renamePlan, error), "default rename plan prepares");
    check(renamePlan.renamedCopies.size() == 2, "both incoming collisions are preserved");
    check(renamePlan.renamedCopies.size() >= 2 &&
              renamePlan.renamedCopies[0].toRelative == "report_collision_0002.txt" &&
              renamePlan.renamedCopies[1].toRelative == "report_collision_0003.txt",
          "collision numbers skip occupied names");

    for (std::size_t i = 0; i < renamePlan.sources.size(); ++i) {
        const std::string exclude = writeExcludeFile(renamePlan.exclusions[i]);
        check(runRsync(buildRsyncArgs(renamePlan.sources[i], renamePlan.destination, exclude,
                                      RsyncMode::Merge), {}) == 0,
              "base merge with renamed conflicts succeeds");
        if (!exclude.empty()) std::remove(exclude.c_str());
    }
    for (const auto& copy : renamePlan.renamedCopies) {
        const fs::path source = fs::path(renamePlan.sources[copy.source]) / copy.fromRelative;
        const fs::path destination = fs::path(renamePlan.destination) / copy.toRelative;
        check(runRsync(buildRsyncCopyArgs(source.string(), destination.string(),
                                          copy.kind == EntryKind::Directory,
                                          RsyncMode::Merge), {}) == 0,
              "collision-renamed rsync copy succeeds");
    }
    {
        std::ifstream original(renameOut / "report.txt");
        std::ifstream first(renameOut / "report_collision_0002.txt");
        std::ifstream second(renameOut / "report_collision_0003.txt");
        std::string originalText;
        std::string firstText;
        std::string secondText;
        original >> originalText;
        first >> firstText;
        second >> secondText;
        check(originalText == "destination-copy", "destination keeps original name");
        check(firstText == "first-copy", "first incoming collision is renamed");
        check(secondText == "second-copy", "second incoming collision is renamed");
    }
    fs::remove_all(root);
    if (failures == 0) std::cout << "all merge plan checks passed\n";
    return failures == 0 ? 0 : 1;
}
