#include "rsync_process.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

std::string escapePattern(const std::string& path) {
    std::string escaped;
    escaped.reserve(path.size() + 8);
    for (const char c : path) {
        if (c == '\\' || c == '*' || c == '?' || c == '[') escaped += '\\';
        escaped += c;
    }
    return escaped;
}

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

bool parseProgress(const std::string& line, RsyncProgress& progress) {
    std::vector<std::string> tokens;
    std::string token;
    for (const char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) tokens.push_back(std::move(token));
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(std::move(token));
    if (tokens.size() < 4 || tokens[1].empty() || tokens[1].back() != '%') return false;
    progress.fraction = static_cast<float>(std::atoi(tokens[1].c_str())) / 100.0f;
    progress.transferred = tokens[0];
    progress.speed = tokens[2];
    progress.eta = tokens[3];
    return true;
}

}  // namespace

std::vector<std::string> buildRsyncArgs(const std::string& source,
                                        const std::string& destination,
                                        const std::string& excludeFile,
                                        RsyncMode mode) {
    std::vector<std::string> args{"rsync", "-a", "--info=progress2", "--no-inc-recursive"};
    if (mode == RsyncMode::DryRun) {
        args.push_back("--dry-run");
        args.push_back("--itemize-changes");
    }
    if (!excludeFile.empty()) {
        args.push_back("--from0");
        args.push_back("--exclude-from=" + excludeFile);
    }
    args.push_back(source.back() == '/' ? source : source + '/');
    args.push_back(destination.back() == '/' ? destination : destination + '/');
    return args;
}

std::string displayCommand(const std::vector<std::string>& args) {
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        const bool quote = arg.find_first_of(" \t\n\"'\\$") != std::string::npos;
        if (quote) command += '"';
        for (const char c : arg) {
            if (quote && (c == '"' || c == '\\' || c == '$')) command += '\\';
            command += c;
        }
        if (quote) command += '"';
    }
    return command;
}

int runRsync(const std::vector<std::string>& args, const RsyncCallbacks& callbacks) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    int pipes[2];
    if (pipe(pipes) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipes[1]);
    if (callbacks.onStarted) callbacks.onStarted(pid);
    std::string pending;
    char buffer[4096];
    const auto flush = [&]() {
        const std::string line = trim(pending);
        pending.clear();
        if (line.empty()) return;
        RsyncProgress progress;
        if (parseProgress(line, progress)) {
            if (callbacks.onProgress) callbacks.onProgress(progress);
        } else if (callbacks.onLine) {
            callbacks.onLine(line);
        }
    };

    ssize_t count = 0;
    while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] == '\r' || buffer[i] == '\n') flush();
            else pending += buffer[i];
        }
    }
    flush();
    close(pipes[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::string writeExcludeFile(const std::vector<std::string>& paths) {
    if (paths.empty()) return {};
    char name[] = "/tmp/merge-folders-exclude-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) return {};
    close(fd);
    std::ofstream output(name, std::ios::trunc | std::ios::binary);
    if (!output) {
        std::remove(name);
        return {};
    }
    for (const auto& path : paths) {
        output << '/' << escapePattern(path);
        output.put('\0');
    }
    if (!output) {
        std::remove(name);
        return {};
    }
    return name;
}
