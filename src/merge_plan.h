#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class EntryKind { Directory, File, Symlink, Other };

struct Candidate {
    int source = -1;  // -1 is the existing destination.
    EntryKind kind = EntryKind::Other;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
};

inline constexpr int kRenameCollisions = -2;

struct Conflict {
    std::string relativePath;
    std::vector<Candidate> candidates;
    // Candidate index, or kRenameCollisions to preserve every copy.
    int selected = kRenameCollisions;
};

struct RenamedCopy {
    int source = -1;
    EntryKind kind = EntryKind::Other;
    std::string fromRelative;
    std::string toRelative;
};

struct MergePlan {
    std::vector<std::string> sources;
    std::string destination;
    std::vector<Conflict> conflicts;
    std::vector<std::vector<std::string>> exclusions;
    std::vector<std::string> destinationRemovals;
    std::vector<RenamedCopy> renamedCopies;
    std::size_t entryCount = 0;
};

const char* entryKindName(EntryKind kind);
std::string validateInputs(const std::vector<std::string>& sources,
                           const std::string& destination);

// Builds a union manifest without following directory symlinks. Plain
// directories merge; overlapping leaves and shape clashes become conflicts.
MergePlan inspectMerge(const std::vector<std::string>& sources,
                       const std::string& destination, std::string& error,
                       const std::function<bool()>& cancelled = {});

bool allConflictsResolved(const MergePlan& plan);

// Converts conflict choices into excludes, destination replacements, and
// collision-renamed copies. Generated names never overlap an existing entry.
bool prepareMerge(MergePlan& plan, std::string& error);

// Rsync removes moved non-directory entries. This removes only directories
// left empty afterwards, including an empty source root.
bool removeEmptySourceDirectories(const std::vector<std::string>& sources,
                                  std::string& error);

std::string formatSize(std::uintmax_t bytes);
std::string formatTime(std::filesystem::file_time_type time);
