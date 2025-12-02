module;

// compat
#include "compat.h"
#include "viz/compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdlib>
#include <memory>
#include <utility>

module sim_estab.viz;

import sim_estab;

using namespace sim_estab::core::log;

namespace sim_estab::viz {

// Debug check function implementation
void VizContext::check_impl() const noexcept {
    if (impl_ == nullptr) [[unlikely]] {
        sim_estab_log("sim_estab.viz", severity_level::critical,
                      "FATAL: Accessing uninitialized VizContext - this is a programming error");
        std::abort();
    }
}

// Implementation struct holding SDL resources
struct VizContext::Impl {
    SDL_Window *window        = nullptr;
    SDL_GPUDevice *gpu_device = nullptr;
    int width                 = 0;
    int height                = 0;
    std::string title;
    bool sdl_initialized = false;

    Impl() = default;

    // Non-copyable, non-movable (managed by VizContext)
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&)                 = delete;
    Impl& operator=(Impl&&)      = delete;

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
VizContext::VizContext(int width, int height, std::string_view title, bool resizable) {
    // Ensure our buffer is large enough
    static_assert(sizeof(Impl) <= ImplSize, "Impl size exceeds SBO buffer size");
    static_assert(alignof(Impl) <= ImplAlign, "Impl alignment exceeds SBO buffer alignment");

    // Construct Impl in-place using placement new and cache the pointer
    impl_ = new (impl_buffer_) Impl();

    sim_estab_log("sim_estab.viz", severity_level::info, "Initializing VizContext (", width, "x", height, ", \"", title,
                  "\")");

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.viz", severity_level::error, "SDL_Init failed: ", error);
        impl_->~Impl();  // Explicitly destroy on error
        impl_ = nullptr; // Mark as destroyed
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
        // Explicitly call destructor for placement new object
        impl_->~Impl();
        impl_ = nullptr;
    }
}

// Move constructor
VizContext::VizContext(VizContext&& other) noexcept
    : impl_(nullptr) // Initialize before constructing
{
    // Check if other is initialized
    if (!other.impl_) {
        // Other is not initialized, so we also remain uninitialized
        return;
    }

    // Construct new Impl in our buffer and cache the pointer
    impl_ = new (impl_buffer_) Impl();

    // Transfer ownership (shallow copy of pointers)
    impl_->window          = other.impl_->window;
    impl_->gpu_device      = other.impl_->gpu_device;
    impl_->width           = other.impl_->width;
    impl_->height          = other.impl_->height;
    impl_->title           = std::move(other.impl_->title);
    impl_->sdl_initialized = other.impl_->sdl_initialized;

    // Null out the source to prevent double-free
    other.impl_->window          = nullptr;
    other.impl_->gpu_device      = nullptr;
    other.impl_->sdl_initialized = false;

    // Destroy source Impl and mark as destroyed
    other.impl_->~Impl();
    other.impl_ = nullptr;
}

// Move assignment
VizContext& VizContext::operator=(VizContext&& other) noexcept {
    if (this != &other) {
        // Destroy current impl if it exists
        if (impl_) {
            impl_->~Impl();
            impl_ = nullptr;
        }

        // Check if other is initialized
        if (!other.impl_) {
            // Other is not initialized, so we also remain uninitialized
            return *this;
        }

        // Construct new Impl in our buffer and cache the pointer
        impl_ = new (impl_buffer_) Impl();

        // Transfer ownership
        impl_->window          = other.impl_->window;
        impl_->gpu_device      = other.impl_->gpu_device;
        impl_->width           = other.impl_->width;
        impl_->height          = other.impl_->height;
        impl_->title           = std::move(other.impl_->title);
        impl_->sdl_initialized = other.impl_->sdl_initialized;

        // Null out the source
        other.impl_->window          = nullptr;
        other.impl_->gpu_device      = nullptr;
        other.impl_->sdl_initialized = false;

        // Destroy source Impl and mark as destroyed
        other.impl_->~Impl();
        other.impl_ = nullptr;
    }
    return *this;
}

// Window state control
void VizContext::show() {
    check_impl();
    if (impl_->window) {
        SDL_ShowWindow(impl_->window);
    }
}

void VizContext::hide() {
    check_impl();
    if (impl_->window) {
        SDL_HideWindow(impl_->window);
    }
}

void VizContext::minimize() {
    check_impl();
    if (impl_->window) {
        SDL_MinimizeWindow(impl_->window);
    }
}

void VizContext::maximize() {
    check_impl();
    if (impl_->window) {
        SDL_MaximizeWindow(impl_->window);
    }
}

void VizContext::restore() {
    check_impl();
    if (impl_->window) {
        SDL_RestoreWindow(impl_->window);
    }
}

// Window state queries
bool VizContext::is_visible() const {
    check_impl();
    if (impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_HIDDEN) == 0;
    }
    return false;
}

bool VizContext::is_minimized() const {
    check_impl();
    if (impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED) != 0;
    }
    return false;
}

bool VizContext::is_maximized() const {
    check_impl();
    if (impl_->window) {
        return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MAXIMIZED) != 0;
    }
    return false;
}

// Window property queries
int VizContext::get_width() const {
    check_impl();
    if (impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return w;
    }
    return impl_->width;
}

int VizContext::get_height() const {
    check_impl();
    if (impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return h;
    }
    return impl_->height;
}

std::string VizContext::get_title() const {
    check_impl();
    if (impl_->window) {
        const char *title = SDL_GetWindowTitle(impl_->window);
        return title ? std::string(title) : std::string();
    }
    return impl_->title;
}

void VizContext::set_title(std::string_view title) {
    check_impl();
    impl_->title = title;
    if (impl_->window) {
        SDL_SetWindowTitle(impl_->window, impl_->title.c_str());
    }
}

// GPU queries
GPUDeviceInfo VizContext::get_gpu_info() const {
    check_impl();
    GPUDeviceInfo info{};
    info.backend = GPUBackend::Unknown;

    if (impl_->gpu_device) {
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

// Inline implementation for has_gpu_device
bool VizContext::has_gpu_device() const noexcept {
    check_impl();
    return impl_->gpu_device != nullptr;
}

} // namespace sim_estab::viz