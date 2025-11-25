module;

// compat
#include "compat.h"
#include "viz/compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <memory>
#include <utility>

module sim_estab.viz;

import sim_estab;

using namespace sim_estab::core::log;

namespace sim_estab::viz {

// Implementation struct holding SDL resources
struct VizContext::Impl {
    SDL_Window *window        = nullptr;
    SDL_GPUDevice *gpu_device = nullptr;
    int width                 = 0;
    int height                = 0;
    std::string title;
    bool sdl_initialized = false;

    Impl() = default;

    ~Impl() { cleanup(); }

    void cleanup() noexcept {
        // Clean up in reverse order of creation
        if (gpu_device) {
            sim_estab_log("sim_estab.viz", severity_level::debug, "Destroying GPU device");
            SDL_DestroyGPUDevice(gpu_device);
            gpu_device = nullptr;
        }

        if (window) {
            sim_estab_log("sim_estab.viz", severity_level::debug, "Destroying SDL window");
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        if (sdl_initialized) {
            sim_estab_log("sim_estab.viz", severity_level::debug, "Quitting SDL subsystems");
            SDL_Quit();
            sdl_initialized = false;
        }
    }
};

// Constructor
VizContext::VizContext(int width, int height, std::string_view title, bool resizable) : impl_(new Impl()) {
    sim_estab_log("sim_estab.viz", severity_level::info, "Initializing VizContext (", width, "x", height, ", \"", title,
                  "\")");

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.viz", severity_level::error, "SDL_Init failed: ", error);
        delete impl_;
        impl_ = nullptr;
        throw viz_error(std::string("SDL initialization failed: ") + error);
    }
    impl_->sdl_initialized = true;
    sim_estab_log("sim_estab.viz", severity_level::debug, "SDL initialized successfully");

    // Create window flags
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
    if (resizable) {
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_RESIZABLE);
    }

    // Create window
    impl_->window = SDL_CreateWindow(std::string(title).c_str(), width, height, flags);

    if (!impl_->window) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.viz", severity_level::error, "SDL_CreateWindow failed: ", error);
        throw viz_error(std::string("Window creation failed: ") + error);
    }

    impl_->width  = width;
    impl_->height = height;
    impl_->title  = title;

    sim_estab_log("sim_estab.viz", severity_level::debug, "Window created successfully");

    // Create GPU device (prefer Vulkan)
    impl_->gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,
                                            true, // debug mode
                                            nullptr);

    if (!impl_->gpu_device) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.viz", severity_level::error, "SDL_CreateGPUDevice failed: ", error);
        throw viz_error(std::string("GPU device creation failed: ") + error);
    }

    // Claim window for GPU device
    if (!SDL_ClaimWindowForGPUDevice(impl_->gpu_device, impl_->window)) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.viz", severity_level::error, "SDL_ClaimWindowForGPUDevice failed: ", error);
        throw viz_error(std::string("Failed to claim window for GPU: ") + error);
    }

    // Log GPU info
    auto gpu_info            = get_gpu_info();
    const char *backend_name = "Unknown";
    switch (gpu_info.backend) {
    case GPUBackend::Vulkan:
        backend_name = "Vulkan";
        break;
    case GPUBackend::D3D12:
        backend_name = "Direct3D 12";
        break;
    default:
        break;
    }
    sim_estab_log("sim_estab.viz", severity_level::info, "GPU device created with backend: ", backend_name);
}

// Destructor
VizContext::~VizContext() noexcept {
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }
}

// Move constructor
VizContext::VizContext(VizContext&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

// Move assignment
VizContext& VizContext::operator=(VizContext&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            delete impl_;
        }
        impl_       = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

// Window state control
void VizContext::show() {
    if (impl_ && impl_->window) {
        SDL_ShowWindow(impl_->window);
    }
}

void VizContext::hide() {
    if (impl_ && impl_->window) {
        SDL_HideWindow(impl_->window);
    }
}

void VizContext::minimize() {
    if (impl_ && impl_->window) {
        SDL_MinimizeWindow(impl_->window);
    }
}

void VizContext::maximize() {
    if (impl_ && impl_->window) {
        SDL_MaximizeWindow(impl_->window);
    }
}

void VizContext::restore() {
    if (impl_ && impl_->window) {
        SDL_RestoreWindow(impl_->window);
    }
}

// Window state queries
bool VizContext::is_visible() const {
    if (impl_ && impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_HIDDEN) == 0;
    }
    return false;
}

bool VizContext::is_minimized() const {
    if (impl_ && impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED) != 0;
    }
    return false;
}

bool VizContext::is_maximized() const {
    if (impl_ && impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MAXIMIZED) != 0;
    }
    return false;
}

// Window property queries
int VizContext::get_width() const {
    if (impl_ && impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return w;
    }
    return impl_ ? impl_->width : 0;
}

int VizContext::get_height() const {
    if (impl_ && impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return h;
    }
    return impl_ ? impl_->height : 0;
}

std::string VizContext::get_title() const {
    if (impl_ && impl_->window) {
        const char *title = SDL_GetWindowTitle(impl_->window);
        return title ? std::string(title) : std::string();
    }
    return impl_ ? impl_->title : std::string();
}

void VizContext::set_title(std::string_view title) {
    if (impl_) {
        impl_->title = title;
        if (impl_->window) {
            SDL_SetWindowTitle(impl_->window, impl_->title.c_str());
        }
    }
}

// GPU queries
GPUDeviceInfo VizContext::get_gpu_info() const {
    GPUDeviceInfo info{};
    info.backend = GPUBackend::Unknown;

    if (impl_ && impl_->gpu_device) {
        // Get the backend driver name from SDL
        const char *driver_name = SDL_GetGPUDeviceDriver(impl_->gpu_device);
        if (driver_name) {
            // Map driver name to backend enum
            std::string driver_str(driver_name);
            if (driver_str == "vulkan") {
                info.backend = GPUBackend::Vulkan;
            } else if (driver_str == "direct3d12" || driver_str == "d3d12") {
                info.backend = GPUBackend::D3D12;
            } else {
                info.backend = GPUBackend::Unknown;
            }
        }
    }

    return info;
}

bool VizContext::has_gpu_device() const { return impl_ && impl_->gpu_device != nullptr; }

} // namespace sim_estab::viz