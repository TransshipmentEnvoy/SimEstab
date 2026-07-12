module;

// compat
#include "compat.h"
#include "viz/compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdlib>
#include <memory>
#include <new>
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
    SDL_GPUDevice *gpu_device = nullptr; // Shared device (not owned)
    int width                 = 0;
    int height                = 0;
    bool window_claimed       = false;
    bool sdl_ctx_acquired     = false; // True only after SDL_ctx_acquire succeeded
    std::string title;

    Impl() = default;

    // Non-copyable, non-movable (managed by VizContext)
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&)                 = delete;
    Impl& operator=(Impl&&)      = delete;

    ~Impl() { cleanup(); }

    void cleanup() noexcept {
        // Clean up in reverse order of creation

        // Release window from GPU device before destroying window
        if (gpu_device && window && window_claimed) {
            sim_estab_log("sim_estab.viz", severity_level::debug, "Releasing window from GPU device");
            SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
            window_claimed = false;
        }

        if (window) {
            sim_estab_log("sim_estab.viz", severity_level::debug, "Destroying SDL window");
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        // Release our reference to the shared GPU device
        if (gpu_device) {
            sim_estab::core::gpu::GPU_device_release();
            gpu_device = nullptr;
        }

        // Release SDL context reference only if this instance acquired one
        if (sdl_ctx_acquired) {
            sim_estab::core::gpu::SDL_ctx_release();
            sdl_ctx_acquired = false;
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

    try {
        sim_estab_log("sim_estab.viz", severity_level::info, "Initializing VizContext (", width, "x", height, ", \"",
                      title, "\")");

        // Acquire SDL context with video and events subsystems
        try {
            sim_estab::core::gpu::SDL_ctx_acquire(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        } catch (const sim_estab::core::gpu::gpu_error& e) {
            throw viz_error(std::string("SDL initialization failed: ") + e.what());
        }
        impl_->sdl_ctx_acquired = true;
        sim_estab_log("sim_estab.viz", severity_level::debug, "SDL context acquired successfully");

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

        // Acquire shared GPU device
#ifdef NDEBUG
        constexpr bool gpu_debug_mode = false;
#else
        constexpr bool gpu_debug_mode = true;
#endif

        try {
            impl_->gpu_device =
                static_cast<SDL_GPUDevice *>(sim_estab::core::gpu::GPU_device_acquire(gpu_debug_mode));
        } catch (const sim_estab::core::gpu::gpu_error& e) {
            throw viz_error(std::string("GPU device acquisition failed: ") + e.what());
        }

        if (!impl_->gpu_device) {
            throw viz_error("Failed to acquire shared GPU device");
        }

        // Claim window for GPU device
        if (!SDL_ClaimWindowForGPUDevice(impl_->gpu_device, impl_->window)) {
            const char *error = SDL_GetError();
            sim_estab_log("sim_estab.viz", severity_level::error, "SDL_ClaimWindowForGPUDevice failed: ", error);
            throw viz_error(std::string("Failed to claim window for GPU: ") + error);
        }
        impl_->window_claimed = true;

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
    } catch (...) {
        impl_->~Impl();
        impl_ = nullptr;
        throw;
    }
}

// Destructor
VizContext::~VizContext() noexcept {
    if (impl_) {
        // Explicitly call destructor for placement new object
        impl_->~Impl();
        impl_ = nullptr;
    }
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

// Event handling
void VizContext::poll_events() {
    check_impl();
    SDL_PumpEvents();
}

void VizContext::clear(float r, float g, float b) {
    check_impl();
    if (!impl_->gpu_device || !impl_->window) {
        return;
    }

    // Acquire command buffer
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu_device);
    if (!cmd) {
        sim_estab_log("sim_estab.viz", severity_level::warning, "Failed to acquire command buffer: ", SDL_GetError());
        return;
    }

    // Acquire swapchain texture
    SDL_GPUTexture *swapchain_texture = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, impl_->window, &swapchain_texture, nullptr, nullptr)) {
        sim_estab_log("sim_estab.viz", severity_level::warning,
                      "Failed to acquire swapchain texture: ", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    if (swapchain_texture) {
        // Set up render pass with clear color
        SDL_GPUColorTargetInfo color_target{};
        color_target.texture     = swapchain_texture;
        color_target.clear_color = {r, g, b, 1.0f};
        color_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op    = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);
        if (render_pass) {
            // Just end the pass immediately (we only want to clear)
            SDL_EndGPURenderPass(render_pass);
        }
    }

    // Submit command buffer
    SDL_SubmitGPUCommandBuffer(cmd);
}

// Window state queries - return defaults if impl_ is null
bool VizContext::is_visible() const {
    if (!impl_ || !impl_->window) {
        return false;
    }
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_HIDDEN) == 0;
}

bool VizContext::is_minimized() const {
    if (!impl_ || !impl_->window) {
        return false;
    }
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED) != 0;
}

bool VizContext::is_maximized() const {
    if (!impl_ || !impl_->window) {
        return false;
    }
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MAXIMIZED) != 0;
}

// Window property queries - return defaults if impl_ is null
int VizContext::get_width() const {
    if (!impl_) {
        return 0;
    }
    if (impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return w;
    }
    return impl_->width;
}

int VizContext::get_height() const {
    if (!impl_) {
        return 0;
    }
    if (impl_->window) {
        int w, h;
        SDL_GetWindowSize(impl_->window, &w, &h);
        return h;
    }
    return impl_->height;
}

std::string VizContext::get_title() const {
    if (!impl_) {
        return {};
    }
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

// GPU queries - return defaults if impl_ is null
GPUDeviceInfo VizContext::get_gpu_info() const {
    GPUDeviceInfo info{};
    info.backend = GPUBackend::Unknown;

    if (!impl_ || !impl_->gpu_device) {
        return info;
    }

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

    return info;
}

bool VizContext::has_gpu_device() const noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->gpu_device != nullptr;
}

} // namespace sim_estab::viz
