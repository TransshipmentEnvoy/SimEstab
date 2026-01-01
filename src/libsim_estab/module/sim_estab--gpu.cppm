/**
 * @file sim_estab--gpu.cppm
 * @brief GPU compute module interface for SimEstab
 *
 * This module provides headless GPU compute capabilities using SDL3 GPU API.
 * Unlike VizContext which requires a window, ComputeContext provides a pure
 * compute environment for running compute shaders without rendering.
 */

// Global module fragment
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Module declaration
export module sim_estab:gpu;

namespace sim_estab::core::gpu {

/**
 * Exception class for GPU compute-related errors
 */
export class gpu_error : public std::runtime_error {
public:
    explicit gpu_error(const std::string& what) : std::runtime_error(what) {}
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
    /// Device driver name
    std::string driver_name;
};

/**
 * Compute shader format enumeration
 */
export enum class ShaderFormat : uint32_t {
    SPIRV = 0x00000001, // SPIR-V for Vulkan
    DXIL  = 0x00000002, // DXIL for DirectX 12
};

/**
 * Buffer usage flags for compute buffers
 */
export enum class BufferUsage : uint32_t {
    Storage      = 0x00000001, // Storage buffer (SSBO)
    Uniform      = 0x00000002, // Uniform buffer
    TransferSrc  = 0x00000004, // Source for transfers
    TransferDst  = 0x00000008, // Destination for transfers
    ComputeRead  = 0x00000010, // Read by compute shader
    ComputeWrite = 0x00000020, // Write by compute shader
};

// Bitwise operators for BufferUsage
export constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

export constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

export constexpr bool has_flag(BufferUsage value, BufferUsage flag) noexcept {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * Handle types for GPU resources (opaque pointers)
 */
export using ComputeBufferHandle   = void *;
export using ComputePipelineHandle = void *;

// =============================================================================
// SDL Context Management
// =============================================================================
// These functions manage global SDL initialization with reference counting.
// Multiple contexts (VizContext, ComputeContext) can share the same SDL instance.
// Subsystems are only initialized, never shut down until the last reference is released.


/**
 * Forward declaration of SDL type
 */
export using SDL_InitFlags = uint32_t;

/**
 * Acquire SDL context with optional subsystem initialization
 *
 * Increments the global SDL reference count. On first call, initializes SDL.
 * Additional subsystems can be requested and will be initialized via SDL_InitSubSystem.
 * Subsystems are accumulated - once initialized, they remain active until SDL_ctx_release
 * decrements the reference count to zero.
 *
 * @param subsystems SDL subsystem flags to initialize (e.g., SDL_INIT_VIDEO | SDL_INIT_EVENTS)
 * @throws gpu_error if SDL initialization fails
 */
export void SDL_ctx_acquire(SDL_InitFlags subsystems = 0);

/**
 * Release SDL context
 *
 * Decrements the global SDL reference count. When count reaches zero, calls SDL_Quit.
 * Safe to call even if SDL was never initialized (no-op in that case).
 */
export void SDL_ctx_release() noexcept;

/**
 * Check if SDL context is currently initialized
 *
 * @return true if SDL is initialized (reference count > 0)
 */
export [[nodiscard]] bool SDL_ctx_is_initialized() noexcept;

/**
 * ComputeContext manages headless GPU compute resources
 *
 * This class provides a context for running compute shaders without
 * requiring a window or display. It manages GPU device lifecycle,
 * compute pipelines, and data transfer operations.
 *
 * @note This is a move-only type (non-copyable)
 */
export class ComputeContext {
public:
    /**
     * Create a new ComputeContext for headless compute
     *
     * @param prefer_discrete Prefer discrete GPU over integrated if available
     * @throws gpu_error if initialization fails
     */
    explicit ComputeContext(bool prefer_discrete = true);

    /**
     * Destructor - automatically cleans up GPU device and resources
     */
    ~ComputeContext() noexcept;

    // Non-copyable, non-movable (SDL resources cannot be safely moved)
    ComputeContext(const ComputeContext&)            = delete;
    ComputeContext& operator=(const ComputeContext&) = delete;
    ComputeContext(ComputeContext&&)                 = delete;
    ComputeContext& operator=(ComputeContext&&)      = delete;

    // Device queries
    /**
     * Get information about the GPU device
     */
    [[nodiscard]] GPUDeviceInfo get_device_info() const;

    /**
     * Check if the context has a valid GPU device
     */
    [[nodiscard]] bool has_device() const noexcept;

    // Buffer operations
    /**
     * Create a GPU buffer
     *
     * @param size Buffer size in bytes
     * @param usage Buffer usage flags
     * @return Handle to the created buffer
     * @throws gpu_error if buffer creation fails
     */
    [[nodiscard]] ComputeBufferHandle create_buffer(size_t size, BufferUsage usage);

    /**
     * Destroy a GPU buffer
     *
     * @param buffer Handle to the buffer to destroy
     */
    void destroy_buffer(ComputeBufferHandle buffer);

    /**
     * Upload data to a GPU buffer
     *
     * @param buffer Target buffer handle
     * @param data Data to upload
     * @param offset Offset in the buffer (default: 0)
     * @throws gpu_error if upload fails
     */
    void upload_buffer(ComputeBufferHandle buffer, std::span<const uint8_t> data, size_t offset = 0);

    /**
     * Download data from a GPU buffer
     *
     * @param buffer Source buffer handle
     * @param size Number of bytes to download
     * @param offset Offset in the buffer (default: 0)
     * @return Downloaded data
     * @throws gpu_error if download fails
     */
    [[nodiscard]] std::vector<uint8_t> download_buffer(ComputeBufferHandle buffer, size_t size, size_t offset = 0);

    // Pipeline operations
    /**
     * Create a compute pipeline from SPIR-V bytecode
     *
     * @param spirv_code SPIR-V bytecode for the compute shader
     * @param entry_point Entry point function name (default: "main")
     * @param num_readonly_storage_buffers Number of readonly storage buffers
     * @param num_readwrite_storage_buffers Number of read-write storage buffers
     * @param threadcount_x Thread count in X dimension (default: 1)
     * @param threadcount_y Thread count in Y dimension (default: 1)
     * @param threadcount_z Thread count in Z dimension (default: 1)
     * @return Handle to the created pipeline
     * @throws gpu_error if pipeline creation fails
     */
    [[nodiscard]] ComputePipelineHandle
    create_pipeline(std::span<const uint8_t> spirv_code, std::string_view entry_point = "main",
                    uint32_t num_readonly_storage_buffers = 0, uint32_t num_readwrite_storage_buffers = 0,
                    uint32_t threadcount_x = 1, uint32_t threadcount_y = 1, uint32_t threadcount_z = 1);

    /**
     * Destroy a compute pipeline
     *
     * @param pipeline Handle to the pipeline to destroy
     */
    void destroy_pipeline(ComputePipelineHandle pipeline);

    // Dispatch operations
    /**
     * Dispatch a compute shader
     *
     * @param pipeline Compute pipeline to use
     * @param buffers Buffers to bind (in binding order)
     * @param group_count_x Number of work groups in X dimension
     * @param group_count_y Number of work groups in Y dimension (default: 1)
     * @param group_count_z Number of work groups in Z dimension (default: 1)
     * @throws gpu_error if dispatch fails
     */
    void dispatch(ComputePipelineHandle pipeline, std::span<ComputeBufferHandle> buffers, uint32_t group_count_x,
                  uint32_t group_count_y = 1, uint32_t group_count_z = 1);

    /**
     * Wait for all GPU operations to complete
     */
    void wait_idle();

private:
    // Fast Pimpl implementation using Small Buffer Optimization (SBO)
    struct Impl;
    static constexpr size_t ImplSize  = 512; // Sufficient for GPU resources + metadata
    static constexpr size_t ImplAlign = alignof(void *);

    alignas(ImplAlign) unsigned char impl_buffer_[ImplSize];
    // Cached pointer to Impl - nullptr indicates Impl is not constructed
    Impl *impl_ = nullptr;

    // Check impl_ is valid, aborts on null
    void check_impl() const noexcept;
};

} // namespace sim_estab::core::gpu