#pragma once

#include <string>
#include <vector>

struct DirBrowser {
    bool open = false;
    bool openPopup = false;
    std::string title;
    std::string cwd;
    std::string error;
    std::vector<std::string> directories;
    char path[1024] = {};
};

void openDirBrowser(DirBrowser& browser, const char* title, const std::string& start);
bool drawDirBrowser(DirBrowser& browser, std::string& selected);
