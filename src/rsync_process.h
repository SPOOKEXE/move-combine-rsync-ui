#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

enum class RsyncMode { DryRun, Merge };

struct RsyncProgress {
    float fraction = 0.0f;
    std::string transferred;
    std::string speed;
    std::string eta;
};

struct RsyncCallbacks {
    std::function<void(const RsyncProgress&)> onProgress;
    std::function<void(const std::string&)> onLine;
    std::function<void(pid_t)> onStarted;
};

std::vector<std::string> buildRsyncArgs(const std::string& source,
                                        const std::string& destination,
                                        const std::string& excludeFile,
                                        RsyncMode mode);
std::vector<std::string> buildRsyncCopyArgs(const std::string& source,
                                            const std::string& destination,
                                            bool directory, RsyncMode mode);
std::string displayCommand(const std::vector<std::string>& args);
int runRsync(const std::vector<std::string>& args, const RsyncCallbacks& callbacks);

// Writes anchored rsync filter patterns and returns the temporary filename.
std::string writeExcludeFile(const std::vector<std::string>& paths);
