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
export class viz_error : public std::runtime_error {
public:
    explicit viz_error(const std::string& what) : std::runtime_error(what) {}
};

/**
 * GPU backend type enumeration
 */
export enum class GPUBackend : int { Vulkan = 0, D3D12 = 1, Unknown = -1 };

/**
 * GPU device information structure
 */
export struct GPUDeviceInfo {
    /// Graphics API backend being used (Vulkan, D3D12, etc.)
    GPUBackend backend;
};

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

    // Non-copyable
    VizContext(const VizContext&)            = delete;
    VizContext& operator=(const VizContext&) = delete;

    // Movable
    VizContext(VizContext&& other) noexcept;
    VizContext& operator=(VizContext&& other) noexcept;

    // Window state control
    void show();
    void hide();
    void minimize();
    void maximize();
    void restore();

    // Window state queries
    bool is_visible() const;
    bool is_minimized() const;
    bool is_maximized() const;

    // Window property queries
    int get_width() const;
    int get_height() const;
    std::string get_title() const;
    void set_title(std::string_view title);

    // GPU queries
    GPUDeviceInfo get_gpu_info() const;

    [[nodiscard]] inline bool has_gpu_device() const noexcept;

private:
    // Fast Pimpl implementation using Small Buffer Optimization (SBO)
    // Avoids heap allocation for the Impl struct
    struct Impl;
    static constexpr size_t ImplSize  = 256; // Sufficient for SDL pointers + metadata
    static constexpr size_t ImplAlign = alignof(void *);

    alignas(ImplAlign) unsigned char impl_buffer_[ImplSize];
    // Cached pointer to Impl - avoids repeated std::launder calls
    // nullptr indicates Impl is not constructed
    Impl *impl_ = nullptr;

    // Check impl_ is valid, aborts on null (always checked, even in release)
    void check_impl() const noexcept;
};

} // namespace sim_estab::viz