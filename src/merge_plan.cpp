#include "merge_plan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

EntryKind kindOf(const fs::file_status& status) {
    if (fs::is_directory(status)) return EntryKind::Directory;
    if (fs::is_regular_file(status)) return EntryKind::File;
    if (fs::is_symlink(status)) return EntryKind::Symlink;
    return EntryKind::Other;
}

Candidate describe(const fs::directory_entry& entry, int source) {
    Candidate candidate;
    candidate.source = source;
    std::error_code ec;
    candidate.kind = kindOf(entry.symlink_status(ec));
    if (candidate.kind == EntryKind::File) candidate.size = entry.file_size(ec);
    ec.clear();
    candidate.modified = entry.last_write_time(ec);
    return candidate;
}

bool samePathOrNested(const fs::path& left, const fs::path& right) {
    auto a = left.begin();
    auto b = right.begin();
    while (a != left.end() && b != right.end() && *a == *b) {
        ++a;
        ++b;
    }
    return a == left.end() || b == right.end();
}

std::string normalized(const std::string& raw, std::error_code& ec) {
    fs::path path = fs::weakly_canonical(fs::path(raw), ec);
    return ec ? std::string{} : path.lexically_normal().string();
}

bool needsConflict(const std::vector<Candidate>& candidates) {
    if (candidates.size() < 2) return false;
    return std::any_of(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
        return candidate.kind != EntryKind::Directory;
    });
}

bool isShapeClash(const std::vector<Candidate>& candidates) {
    if (candidates.empty()) return false;
    const EntryKind first = candidates.front().kind;
    return std::any_of(candidates.begin() + 1, candidates.end(),
                       [first](const Candidate& candidate) {
                           return candidate.kind != first;
                       });
}

bool pathCoveredBy(const std::string& path, const std::string& parent) {
    if (path == parent) return true;
    return path.size() > parent.size() && path.compare(0, parent.size(), parent) == 0 &&
           path[parent.size()] == '/';
}

void reducePaths(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::vector<std::string> reduced;
    for (const auto& path : paths) {
        const bool covered = std::any_of(reduced.begin(), reduced.end(), [&](const auto& parent) {
            return pathCoveredBy(path, parent);
        });
        if (!covered) reduced.push_back(path);
    }
    paths = std::move(reduced);
}

bool entryExists(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && status.type() != fs::file_type::not_found;
}

bool occupiedInInputs(const MergePlan& plan, const std::string& relative) {
    if (entryExists(fs::path(plan.destination) / relative)) return true;
    return std::any_of(plan.sources.begin(), plan.sources.end(), [&](const std::string& source) {
        return entryExists(fs::path(source) / relative);
    });
}

std::string collisionName(const std::string& relative, unsigned number) {
    const fs::path path(relative);
    const fs::path filename = path.filename();
    const std::string extension = filename.extension().string();
    const std::string stem = filename.stem().string();
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_collision_%04u", number);
    return (path.parent_path() / (stem + suffix + extension)).generic_string();
}

}  // namespace

const char* entryKindName(EntryKind kind) {
    switch (kind) {
        case EntryKind::Directory: return "folder";
        case EntryKind::File: return "file";
        case EntryKind::Symlink: return "link";
        case EntryKind::Other: return "other";
    }
    return "other";
}

std::string validateInputs(const std::vector<std::string>& sources,
                           const std::string& destination) {
    if (sources.empty()) return "add at least one source folder";
    if (destination.empty()) return "choose a destination folder";

    std::error_code ec;
    const std::string dest = normalized(destination, ec);
    if (ec || dest.empty()) return "destination path is not valid";
    const bool destinationExists = fs::exists(dest, ec);
    if (ec || (destinationExists && !fs::is_directory(dest, ec))) {
        return "destination must be a folder";
    }
    if (!destinationExists && !fs::is_directory(fs::path(dest).parent_path(), ec)) {
        return "destination parent must be an existing folder";
    }

    std::set<std::string> seen;
    std::vector<fs::path> normalizedSources;
    for (const auto& raw : sources) {
        ec.clear();
        const std::string source = normalized(raw, ec);
        if (ec || source.empty() || !fs::is_directory(source, ec)) {
            return "every source must be an existing readable folder";
        }
        if (!seen.insert(source).second) return "the same source folder was added twice";
        if (samePathOrNested(source, dest)) {
            return "source and destination folders must not contain each other";
        }
        normalizedSources.emplace_back(source);
    }
    for (std::size_t i = 0; i < normalizedSources.size(); ++i) {
        for (std::size_t j = i + 1; j < normalizedSources.size(); ++j) {
            if (samePathOrNested(normalizedSources[i], normalizedSources[j])) {
                return "source folders must not contain each other";
            }
        }
    }
    return {};
}

MergePlan inspectMerge(const std::vector<std::string>& sources,
                       const std::string& destination, std::string& error,
                       const std::function<bool()>& cancelled) {
    MergePlan plan;
    plan.sources = sources;
    plan.destination = destination;
    error = validateInputs(sources, destination);
    if (!error.empty()) return plan;

    std::map<std::string, std::vector<Candidate>> entries;
    const auto addTree = [&](const std::string& root, int source) -> std::string {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        fs::recursive_directory_iterator end;
        if (ec) return "cannot read " + root + ": " + ec.message();
        while (it != end) {
            if (cancelled && cancelled()) return "inspection cancelled";
            const fs::directory_entry entry = *it;
            // Iterator paths are already rooted below root. A lexical relative
            // path avoids resolving a symlink that points outside the tree.
            const std::string relative = entry.path().lexically_relative(root).generic_string();
            if (relative.empty() || relative.starts_with("../")) {
                return "cannot inspect path outside " + root;
            }
            entries[relative].push_back(describe(entry, source));
            ++plan.entryCount;
            it.increment(ec);
            if (ec) return "cannot read " + root + ": " + ec.message();
        }
        return {};
    };

    std::error_code destinationError;
    if (fs::exists(destination, destinationError)) {
        error = addTree(destination, -1);
        if (!error.empty()) return plan;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        error = addTree(sources[i], static_cast<int>(i));
        if (!error.empty()) return plan;
    }

    std::vector<std::string> shapeClashes;
    for (auto& [path, candidates] : entries) {
        const bool belowShapeClash = std::any_of(
            shapeClashes.begin(), shapeClashes.end(),
            [&](const std::string& parent) { return pathCoveredBy(path, parent); });
        if (belowShapeClash) continue;
        if (needsConflict(candidates)) {
            plan.conflicts.push_back({.relativePath = path,
                                      .candidates = std::move(candidates),
                                      .selected = kRenameCollisions});
            if (isShapeClash(plan.conflicts.back().candidates)) shapeClashes.push_back(path);
        }
    }
    return plan;
}

bool allConflictsResolved(const MergePlan& plan) {
    return std::all_of(plan.conflicts.begin(), plan.conflicts.end(), [](const Conflict& conflict) {
        return conflict.selected == kRenameCollisions ||
               (conflict.selected >= 0 &&
                conflict.selected < static_cast<int>(conflict.candidates.size()));
    });
}

bool prepareMerge(MergePlan& plan, std::string& error) {
    if (!allConflictsResolved(plan)) {
        error = "resolve every conflict before merging";
        return false;
    }

    plan.exclusions.assign(plan.sources.size(), {});
    plan.destinationRemovals.clear();
    plan.renamedCopies.clear();
    std::set<std::string> allocated;
    for (const auto& conflict : plan.conflicts) {
        if (conflict.selected == kRenameCollisions) {
            const bool destinationKeepsOriginal = std::any_of(
                conflict.candidates.begin(), conflict.candidates.end(),
                [](const Candidate& candidate) { return candidate.source < 0; });
            int originalSource = -1;
            if (!destinationKeepsOriginal) {
                for (const auto& candidate : conflict.candidates) {
                    if (candidate.source >= 0 &&
                        (originalSource < 0 || candidate.source < originalSource)) {
                        originalSource = candidate.source;
                    }
                }
            }

            unsigned collision = 1;
            for (const auto& candidate : conflict.candidates) {
                if (candidate.source < 0 || candidate.source == originalSource) continue;
                plan.exclusions[static_cast<std::size_t>(candidate.source)].push_back(
                    conflict.relativePath);
                std::string renamed;
                do {
                    renamed = collisionName(conflict.relativePath, collision++);
                } while (occupiedInInputs(plan, renamed) || allocated.contains(renamed));
                allocated.insert(renamed);
                plan.renamedCopies.push_back({.source = candidate.source,
                                              .kind = candidate.kind,
                                              .fromRelative = conflict.relativePath,
                                              .toRelative = std::move(renamed)});
            }
            continue;
        }

        const Candidate& winner = conflict.candidates[conflict.selected];
        for (const auto& candidate : conflict.candidates) {
            if (candidate.source >= 0 && candidate.source != winner.source) {
                plan.exclusions[static_cast<std::size_t>(candidate.source)].push_back(
                    conflict.relativePath);
            }
        }

        const auto destination = std::find_if(
            conflict.candidates.begin(), conflict.candidates.end(),
            [](const Candidate& candidate) { return candidate.source == -1; });
        if (destination != conflict.candidates.end() && winner.source >= 0 &&
            destination->kind != winner.kind) {
            plan.destinationRemovals.push_back(conflict.relativePath);
        }
    }
    for (auto& paths : plan.exclusions) reducePaths(paths);
    reducePaths(plan.destinationRemovals);
    error.clear();
    return true;
}

bool removeEmptySourceDirectories(const std::vector<std::string>& sources,
                                  std::string& error) {
    for (const auto& source : sources) {
        std::error_code ec;
        if (!fs::exists(source, ec)) continue;
        if (ec) {
            error = "cannot inspect source after move: " + source + ": " + ec.message();
            return false;
        }

        std::vector<fs::path> directories;
        fs::recursive_directory_iterator iterator(
            source, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        if (ec) {
            error = "cannot inspect source after move: " + source + ": " + ec.message();
            return false;
        }
        while (iterator != end) {
            const fs::file_status status = iterator->symlink_status(ec);
            if (ec) {
                error = "cannot inspect source after move: " + iterator->path().string() +
                        ": " + ec.message();
                return false;
            }
            if (fs::is_directory(status)) directories.push_back(iterator->path());
            iterator.increment(ec);
            if (ec) {
                error = "cannot inspect source after move: " + source + ": " + ec.message();
                return false;
            }
        }

        std::sort(directories.begin(), directories.end(), [](const fs::path& left,
                                                              const fs::path& right) {
            return left.native().size() > right.native().size();
        });
        for (const auto& directory : directories) {
            fs::remove(directory, ec);
            if (ec) {
                error = "cannot remove empty source folder: " + directory.string() +
                        ": " + ec.message();
                return false;
            }
        }
        fs::remove(source, ec);
        if (ec) {
            error = "cannot remove empty source folder: " + source + ": " + ec.message();
            return false;
        }
    }
    error.clear();
    return true;
}

std::string formatSize(std::uintmax_t bytes) {
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value,
                  units[unit]);
    return buffer;
}

std::string formatTime(fs::file_time_type time) {
    if (time == fs::file_time_type{}) return "-";
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
    localtime_r(&value, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}
