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

struct Conflict {
    std::string relativePath;
    std::vector<Candidate> candidates;
    int selected = -1;  // Candidate index. Every conflict starts unresolved.
};

struct MergePlan {
    std::vector<std::string> sources;
    std::string destination;
    std::vector<Conflict> conflicts;
    std::vector<std::vector<std::string>> exclusions;
    std::vector<std::string> destinationRemovals;
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

// Converts conflict choices into one anchored exclude list per source and a
// list of incompatible destination entries that must be removed before rsync.
bool prepareMerge(MergePlan& plan, std::string& error);

std::string formatSize(std::uintmax_t bytes);
std::string formatTime(std::filesystem::file_time_type time);
