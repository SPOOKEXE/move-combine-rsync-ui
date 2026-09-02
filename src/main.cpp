#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "app.h"

namespace {

void glfwError(int code, const char* description) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, description);
}

void dropPaths(GLFWwindow* window, int count, const char** paths) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->worker.phase != AppPhase::Setup) return;
    }
    std::error_code ec;
    for (int i = 0; i < count; ++i) {
        if (std::filesystem::is_directory(paths[i], ec)) state->sources.emplace_back(paths[i]);
        ec.clear();
    }
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1100, 760, "Merge folders", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    AppState state;
    glfwSetWindowUserPointer(window, &state);
    glfwSetDropCallback(window, dropPaths);
    while (!glfwWindowShouldClose(window)) {
        AppPhase phase;
        {
            std::lock_guard lock(state.mutex);
            phase = state.worker.phase;
        }
        const bool busy = phase == AppPhase::Scanning || phase == AppPhase::Merging;
        glfwWaitEventsTimeout(busy ? 0.08 : 0.5);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawApp(state);
        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    shutdownApp(state);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
