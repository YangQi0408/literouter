#include <eui_neo.h>

#include "eui/detail/dsl_app_impl.h"

#include "core/input/input_state.h"
#include "core/platform/platform.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "app_state.h"

#include "components/shell.h"
#include "components/theme.h"
#include "components/widgets.h"
#include "pages/logs.h"
#include "pages/overlays.h"
#include "pages/overview.h"
#include "pages/providers.h"
#include "pages/routes.h"
#include "pages/settings.h"

namespace app {

const DslAppConfig& dslAppConfig() {
    static DslAppConfig config = [] {
        DslAppConfig cfg = DslAppConfig{}
            .title("literouter console")
            .pageId("literouter_gui")
            .clearColor({0.055f, 0.063f, 0.080f, 1.0f})
            .windowSize(1440, 900)
            .fps(30.0)
            .textFont(lr_gui::uiFontPath())
            .showDebugStatsInTitle(false);
        cfg.onKeyEvent([](const eui::KeyEvent& event) {
            if (!event.isDown()) {
                return;
            }
            if (event.modifiers.shortcut()) {
                if (event.key == core::InputKey::Equal || event.key == core::InputKey::NumpadAdd) {
                    lr_gui::appState().zoomIn();
                } else if (event.key == core::InputKey::Minus || event.key == core::InputKey::NumpadSubtract) {
                    lr_gui::appState().zoomOut();
                } else if (event.key == core::InputKey::Digit0 || event.key == core::InputKey::Numpad0) {
                    lr_gui::appState().resetZoom();
                }
            }
        });
        return cfg;
    }();
    config.uiScale(lr_gui::appState().currentScale());
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    lr_gui::AppState* state = &lr_gui::appState();
    const float width = screen.width;
    const float height = screen.height;
    const float sidebarWidth = lr_gui::layout::sidebarWidth;
    const float contentX = sidebarWidth;
    const float contentWidth = std::max(0.0f, width - sidebarWidth);

    ui.stack("root").size(width, height).content([&] {
        ui.rect("root.bg").fill().color(lr_gui::palette().canvas).build();

        // A node carrying .onFrame() keeps the runtime animating; the callback is
        // the console's 10Hz telemetry tick and its smoke-mode page timer. It
        // never blocks: snapshot()/logsSince() are the only server calls here,
        // and start/stop/probe run on the AppState worker thread.
        ui.stack("loop.driver")
            .size(1.0f, 1.0f)
            .onFrame([state](float deltaSeconds) { state->onTick(deltaSeconds); })
            .build();

        lr_gui::composeSidebar(ui, sidebarWidth, height);

        const float topBarHeight = lr_gui::layout::topBarHeight;
        lr_gui::composeTopBar(ui, contentX, 0.0f, contentWidth, topBarHeight);
        const float bannerHeight = lr_gui::composeBanners(ui, contentX, topBarHeight, contentWidth);
        const float bodyY = topBarHeight + bannerHeight;
        const float bodyHeight = std::max(0.0f, height - bodyY);

        switch (state->page) {
            case lr_gui::Page::Overview:
                lr_gui::composeOverview(ui, contentX, bodyY, contentWidth, bodyHeight);
                break;
            case lr_gui::Page::Providers:
                lr_gui::composeProviders(ui, contentX, bodyY, contentWidth, bodyHeight);
                break;
            case lr_gui::Page::Routes:
                lr_gui::composeRoutes(ui, contentX, bodyY, contentWidth, bodyHeight);
                break;
            case lr_gui::Page::Logs:
                lr_gui::composeLogs(ui, contentX, bodyY, contentWidth, bodyHeight);
                break;
            case lr_gui::Page::Settings:
                lr_gui::composeSettings(ui, contentX, bodyY, contentWidth, bodyHeight);
                break;
        }

        lr_gui::composeOverlays(ui, screen);
    }).build();
}

} // namespace app

namespace {

float dpiScaleOf(GLFWwindow* window) {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return (scaleX + scaleY) * 0.5f;
}

float pointerScaleOf(GLFWwindow* window) {
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return 1.0f;
    }
    return 0.5f * (static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth) +
                   static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight));
}

struct LoopStats {
    int frames = 0;
    long long rectDraws = 0;
    long long textDraws = 0;
};

int runWindow() {
    lr_gui::AppState& state = lr_gui::appState();
    state.init();

    core::platform::repairCurrentWorkingDirectory();
    core::render::initializeRenderBackendLoader();

    glfwSetErrorCallback([](int code, const char* description) {
        std::println("glfw error {}: {}", code, description != nullptr ? description : "");
    });

    if (!glfwInit()) {
        std::println("glfwInit() failed");
        return 10;
    }

    const app::DslAppConfig& config = app::dslAppConfig();

    core::window::WindowCreateRequest request;
    request.width = config.windowWidthValue;
    request.height = config.windowHeightValue;
    request.title = config.titleValue.c_str();
    request.renderApi = core::render::windowRenderApi();

    auto* window = static_cast<GLFWwindow*>(core::window::createWindow(request));
    if (window == nullptr) {
        std::println("core::window::createWindow() failed");
        glfwTerminate();
        return 11;
    }

    auto backend = core::render::createRenderBackend(window);
    if (!backend || !backend->initialize()) {
        std::println("core::render::createRenderBackend() failed");
        core::window::destroyWindow(window);
        glfwTerminate();
        return 12;
    }

    if (!app::initialize(window)) {
        std::println("app::initialize() failed");
        backend.reset();
        core::window::destroyWindow(window);
        glfwTerminate();
        return 13;
    }

    if (state.smokeMode) {
        std::println("literouter console: smoke mode — cycling {} pages, {} frames each",
                     lr_gui::kPageCount, state.smokeFramesPerPage);
    }

    LoopStats loop;
    const double frameInterval = config.fpsValue > 0.0 ? 1.0 / config.fpsValue : 0.0;
    double lastFrameTime = core::window::timeSeconds();
    double nextFrameTime = lastFrameTime;
    const int budget = state.smokeMode ? lr_gui::smokeFrameBudget() : 0;
    float lastScale = state.currentScale();

    while (!glfwWindowShouldClose(window) && (budget <= 0 || loop.frames < budget)) {
        glfwPollEvents();

        const double now = core::window::timeSeconds();
        const float deltaSeconds = static_cast<float>(now - lastFrameTime);
        lastFrameTime = now;

        const float curScale = state.currentScale();
        if (std::abs(curScale - lastScale) > 1e-4f) {
            lastScale = curScale;
            app::detail::requestFullPaint();
        }

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            glfwWaitEvents();
            lastFrameTime = core::window::timeSeconds();
            nextFrameTime = lastFrameTime;
            continue;
        }

        const float dpiScale = dpiScaleOf(window);
        const float pointerScale = pointerScaleOf(window);

        backend->makeCurrent();
        app::update(window, deltaSeconds, framebufferWidth, framebufferHeight, dpiScale, pointerScale);

        backend->beginFrame({window, core::window::nativeWindowInfo(window),
                             framebufferWidth, framebufferHeight, dpiScale});
        {
            core::render::ScopedRenderBackend scopedBackend(*backend);
            app::render(framebufferWidth, framebufferHeight, dpiScale);
        }
        backend->present();

        core::render::publishRenderFrameStats();
        const core::render::RenderFrameStats& stats = core::render::lastRenderFrameStats();
        loop.rectDraws += stats.rectDraws;
        loop.textDraws += stats.textDraws;
        ++loop.frames;
        state.recordFrame(stats.rectDraws, stats.textDraws);

        nextFrameTime += frameInterval;
        const double slack = nextFrameTime - core::window::timeSeconds();
        if (slack > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(slack));
        } else {
            nextFrameTime = core::window::timeSeconds();
        }
    }

    state.worker.stop();

    core::releaseInputQueue(window);
    backend->makeCurrent();
    backend->releaseRenderCache();
    {
        core::render::ScopedRenderBackend scopedBackend(*backend);
        app::shutdown();
    }
    backend.reset();
    core::window::destroyWindow(window);
    glfwTerminate();

    std::println("literouter console: {} frames, {} rect / {} text draws", loop.frames,
                 loop.rectDraws, loop.textDraws);
    if (state.smokeMode) {
        for (int i = 0; i < lr_gui::kPageCount; ++i) {
            std::println("  page {:<11} rect {:>7}  text {:>7}",
                         lr_gui::pageTitle(static_cast<lr_gui::Page>(i)), state.smokeRect[i],
                         state.smokeText[i]);
        }
    }
    if (loop.rectDraws <= 0 || loop.textDraws <= 0) {
        std::println("no geometry reached the backend");
        return 15;
    }
    return 0;
}

} // namespace

int main() {
    literouter::ensureLocalTimezone();
    return runWindow();
}
