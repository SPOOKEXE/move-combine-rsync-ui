#pragma once

#include <sys/types.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dir_browser.h"
#include "merge_plan.h"

enum class AppPhase { Setup, Scanning, Resolve, Ready, Merging, Done, Failed };
enum class BrowserPurpose { None, AddSource, Destination };

struct WorkerState {
    AppPhase phase = AppPhase::Setup;
    MergePlan plan;
    float progress = 0.0f;
    int currentSource = -1;
    std::string status;
    std::vector<std::string> log;
    pid_t pid = 0;
    bool cancelRequested = false;
};

struct AppState {
    std::vector<std::string> sources;
    std::string destination;
    char sourceInput[1024] = {};
    char destinationInput[1024] = {};
    DirBrowser browser;
    BrowserPurpose browserPurpose = BrowserPurpose::None;

    std::mutex mutex;
    WorkerState worker;
    std::thread thread;
    bool showLog = false;
};

void applyTheme();
void drawApp(AppState& state);
void shutdownApp(AppState& state);
