/**
 * @file viz.cppm
 * @brief Visualization module interface for SimEstab
 *
 * This module provides SDL3-based window management and GPU rendering capabilities.
 */

// Global module fragment
module;

#include <stdexcept>
#include <string>
#include <string_view>

export module sim_estab.viz;

import sim_estab;

namespace sim_estab::viz {

/**
 * Exception class for visualization-related errors
 */
export class viz_error : public sim_estab::core::error::sim_estab_error {
public:
    explicit viz_error(const std::string& what) : sim_estab::core::error::sim_estab_error(what) {}
};

/**
 * GPU device types, shared with ComputeContext
 *
 * Both contexts report the one shared GPU device, so the types are defined once,
 * in sim_estab:gpu, and re-exposed here.
 */
export using sim_estab::core::gpu::GPUBackend;
export using sim_estab::core::gpu::GPUDeviceInfo;

/**
 * VizContext manages SDL window and GPU device lifecycle
 *
 * Each instance creates and owns a single SDL window with associated GPU device.
 * Multiple instances can be created for multi-window applications.
 */
export class VizContext {
public:
    /**
     * Create a new VizContext with specified window properties
     *
     * @throws viz_error if initialization fails
     */
    explicit VizContext(int width = 1280, int height = 720, std::string_view title = "SimEstab Viz",
                        bool resizable = true);

    /**
     * Destructor - automatically cleans up GPU device, window, and SDL subsystems
     */
    ~VizContext() noexcept;

    // Non-copyable, non-movable (SDL resources have complex internal state)
    VizContext(const VizContext&)            = delete;
    VizContext& operator=(const VizContext&) = delete;
    VizContext(VizContext&&)                 = delete;
    VizContext& operator=(VizContext&&)      = delete;

    // Window state control
    void show();
    void hide();
    void minimize();
    void maximize();
    void restore();

    // Event handling
    /**
     * @brief Process pending window events
     *
     * This is essential for Wayland which requires event processing
     * for window compositing. Should be called regularly in the main loop.
     */
    void poll_events();

    /**
     * @brief Clear the window with a color and present
     *
     * Acquires swapchain, clears to a color, and submits.
     * Required for Wayland to actually display the window content.
     *
     * @param r Red component (0.0-1.0)
     * @param g Green component (0.0-1.0)
     * @param b Blue component (0.0-1.0)
     */
    void clear(float r = 0.0f, float g = 0.0f, float b = 0.0f);

    // Window state queries
    [[nodiscard]] bool is_visible() const noexcept;
    [[nodiscard]] bool is_minimized() const noexcept;
    [[nodiscard]] bool is_maximized() const noexcept;

    // Window property queries
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::string title() const noexcept;
    void set_title(std::string_view title);

    // GPU queries
    [[nodiscard]] GPUDeviceInfo device_info() const noexcept;

    [[nodiscard]] bool has_device() const noexcept;

private:
    // Fast Pimpl implementation using Small Buffer Optimization (SBO)
    // Avoids heap allocation for the Impl struct
    struct Impl;
    static constexpr size_t impl_size  = 256; // Sufficient for SDL pointers + metadata
    static constexpr size_t impl_align = alignof(void *);

    alignas(impl_align) unsigned char impl_buffer_[impl_size];
    // Cached pointer to Impl - avoids repeated std::launder calls
    // nullptr indicates Impl is not constructed
    Impl *impl_ = nullptr;

    // Check impl_ is valid, aborts on null (always checked, even in release)
    void check_impl() const noexcept;
};

} // namespace sim_estab::viz