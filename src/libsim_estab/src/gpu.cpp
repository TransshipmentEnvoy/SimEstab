module;

// compat
#include "compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

module sim_estab;

using namespace sim_estab::core::log;

namespace sim_estab::core::gpu {

// =============================================================================
// SDL Context Management Implementation
// =============================================================================

namespace detail {
static std::mutex SDL_ctx_mutex;
static int SDL_ctx_ref_count                        = 0;
static SDL_InitFlags SDL_ctx_initialized_subsystems = 0;
} // namespace detail

void SDL_ctx_acquire(SDL_InitFlags subsystems) {
    std::lock_guard<std::mutex> lock(detail::SDL_ctx_mutex);

    if (detail::SDL_ctx_ref_count == 0) {
        // First acquisition - initialize SDL
        sim_estab_log("sim_estab.gpu", severity_level::info, "Initializing SDL (subsystems=", subsystems, ")");
        if (!SDL_Init(static_cast<SDL_InitFlags>(subsystems))) {
            const char *error = SDL_GetError();
            sim_estab_log("sim_estab.gpu", severity_level::error, "SDL_Init failed: ", error);
            throw gpu_error(std::string("SDL initialization failed: ") + error);
        }
        detail::SDL_ctx_initialized_subsystems = static_cast<SDL_InitFlags>(subsystems);
        sim_estab_log("sim_estab.gpu", severity_level::debug, "SDL initialized successfully");
    } else {
        // Already initialized - check if new subsystems are requested
        auto new_subsystems = static_cast<SDL_InitFlags>(subsystems) & ~detail::SDL_ctx_initialized_subsystems;
        if (new_subsystems != 0) {
            sim_estab_log("sim_estab.gpu", severity_level::debug,
                          "Initializing additional SDL subsystems: ", new_subsystems);
            if (!SDL_InitSubSystem(new_subsystems)) {
                const char *error = SDL_GetError();
                sim_estab_log("sim_estab.gpu", severity_level::error, "SDL_InitSubSystem failed: ", error);
                throw gpu_error(std::string("SDL subsystem initialization failed: ") + error);
            }
            detail::SDL_ctx_initialized_subsystems |= new_subsystems;
        }
    }

    ++detail::SDL_ctx_ref_count;
    sim_estab_log("sim_estab.gpu", severity_level::debug, "SDL context acquired (ref_count=", detail::SDL_ctx_ref_count,
                  ")");
}

void SDL_ctx_release() noexcept {
    std::lock_guard<std::mutex> lock(detail::SDL_ctx_mutex);

    if (detail::SDL_ctx_ref_count <= 0) {
        // Nothing to release
        return;
    }

    --detail::SDL_ctx_ref_count;
    sim_estab_log("sim_estab.gpu", severity_level::debug, "SDL context released (ref_count=", detail::SDL_ctx_ref_count,
                  ")");

    if (detail::SDL_ctx_ref_count == 0) {
        sim_estab_log("sim_estab.gpu", severity_level::info, "Shutting down SDL");
        SDL_Quit();
        detail::SDL_ctx_initialized_subsystems = 0;
    }
}

bool SDL_ctx_is_initialized() noexcept {
    std::lock_guard<std::mutex> lock(detail::SDL_ctx_mutex);
    return detail::SDL_ctx_ref_count > 0;
}

// =============================================================================
// ComputeContext Implementation
// =============================================================================

// Debug check function implementation
void ComputeContext::check_impl() const noexcept {
    if (impl_ == nullptr) [[unlikely]] {
        sim_estab_log("sim_estab.gpu", severity_level::critical,
                      "FATAL: Accessing uninitialized ComputeContext - this is a programming error");
        std::abort();
    }
}

// Implementation struct holding SDL GPU resources
struct ComputeContext::Impl {
    SDL_GPUDevice *gpu_device = nullptr;
    bool prefer_discrete      = true;

    Impl() = default;

    // Non-copyable, non-movable (managed by ComputeContext)
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&)                 = delete;
    Impl& operator=(Impl&&)      = delete;

    ~Impl() { cleanup(); }

    void cleanup() noexcept {
        // Clean up in reverse order of creation
        if (gpu_device) {
            sim_estab_log("sim_estab.gpu", severity_level::debug, "Destroying GPU device");
            SDL_DestroyGPUDevice(gpu_device);
            gpu_device = nullptr;
        }

        // Release SDL context reference
        SDL_ctx_release();
    }
};

// Constructor
ComputeContext::ComputeContext(bool prefer_discrete) {
    // Ensure our buffer is large enough
    static_assert(sizeof(Impl) <= ImplSize, "Impl size exceeds SBO buffer size");
    static_assert(alignof(Impl) <= ImplAlign, "Impl alignment exceeds SBO buffer alignment");

    // Construct Impl in-place using placement new and cache the pointer
    impl_                  = new (impl_buffer_) Impl();
    impl_->prefer_discrete = prefer_discrete;

    sim_estab_log("sim_estab.gpu", severity_level::info,
                  "Initializing ComputeContext (headless, prefer_discrete=", prefer_discrete ? "true" : "false", ")");

    // Acquire SDL context (minimal - no video required for compute-only)
    try {
        SDL_ctx_acquire(0);
    } catch (const gpu_error& e) {
        impl_->~Impl();
        impl_ = nullptr;
        throw;
    }

    // Create GPU device (headless - no window required)
    // Support both SPIR-V (Vulkan) and DXIL (D3D12)
    impl_->gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,
                                            true, // debug mode
                                            nullptr);

    if (!impl_->gpu_device) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.gpu", severity_level::error, "SDL_CreateGPUDevice failed: ", error);
        throw gpu_error(std::string("GPU device creation failed: ") + error);
    }

    // Log GPU info
    auto gpu_info            = get_device_info();
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
    sim_estab_log("sim_estab.gpu", severity_level::info, "GPU device created with backend: ", backend_name);
}

// Destructor
ComputeContext::~ComputeContext() noexcept {
    if (impl_) {
        // Explicitly call destructor for placement new object
        impl_->~Impl();
        impl_ = nullptr;
    }
}

// Device queries
GPUDeviceInfo ComputeContext::get_device_info() const {
    check_impl();
    GPUDeviceInfo info{};
    info.backend = GPUBackend::Unknown;

    if (impl_->gpu_device) {
        // Get the backend driver name from SDL
        const char *driver_name = SDL_GetGPUDeviceDriver(impl_->gpu_device);
        if (driver_name) {
            info.driver_name = driver_name;
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

bool ComputeContext::has_device() const noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->gpu_device != nullptr;
}

// Buffer operations
ComputeBufferHandle ComputeContext::create_buffer(size_t size, BufferUsage usage) {
    check_impl();

    if (size == 0) {
        throw gpu_error("Cannot create buffer with size 0");
    }

    SDL_GPUBufferCreateInfo create_info{};
    create_info.size = static_cast<Uint32>(size);

    // Map our usage flags to SDL usage flags
    SDL_GPUBufferUsageFlags sdl_usage = 0;
    if (has_flag(usage, BufferUsage::Storage) || has_flag(usage, BufferUsage::ComputeRead) ||
        has_flag(usage, BufferUsage::ComputeWrite)) {
        sdl_usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        sdl_usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    }

    create_info.usage = static_cast<SDL_GPUBufferUsageFlags>(sdl_usage);

    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(impl_->gpu_device, &create_info);
    if (!buffer) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.gpu", severity_level::error, "Failed to create GPU buffer: ", error);
        throw gpu_error(std::string("Failed to create GPU buffer: ") + error);
    }

    sim_estab_log("sim_estab.gpu", severity_level::debug, "Created GPU buffer of size ", size);
    return static_cast<ComputeBufferHandle>(buffer);
}

void ComputeContext::destroy_buffer(ComputeBufferHandle buffer) {
    check_impl();
    if (buffer) {
        SDL_ReleaseGPUBuffer(impl_->gpu_device, static_cast<SDL_GPUBuffer *>(buffer));
        sim_estab_log("sim_estab.gpu", severity_level::debug, "Destroyed GPU buffer");
    }
}

void ComputeContext::upload_buffer(ComputeBufferHandle buffer, std::span<const uint8_t> data, size_t offset) {
    check_impl();

    if (!buffer) {
        throw gpu_error("Cannot upload to null buffer");
    }

    if (data.empty()) {
        return; // Nothing to upload
    }

    // Create a transfer buffer for staging
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.size  = static_cast<Uint32>(data.size());
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    SDL_GPUTransferBuffer *transfer_buffer = SDL_CreateGPUTransferBuffer(impl_->gpu_device, &transfer_info);
    if (!transfer_buffer) {
        const char *error = SDL_GetError();
        throw gpu_error(std::string("Failed to create transfer buffer: ") + error);
    }

    // Map and copy data
    void *mapped = SDL_MapGPUTransferBuffer(impl_->gpu_device, transfer_buffer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to map transfer buffer");
    }
    std::memcpy(mapped, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(impl_->gpu_device, transfer_buffer);

    // Create command buffer and copy pass
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu_device);
    if (!cmd) {
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to acquire command buffer");
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);
    if (!copy_pass) {
        SDL_CancelGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to begin copy pass");
    }

    SDL_GPUTransferBufferLocation src{};
    src.transfer_buffer = transfer_buffer;
    src.offset          = 0;

    SDL_GPUBufferRegion dst{};
    dst.buffer = static_cast<SDL_GPUBuffer *>(buffer);
    dst.offset = static_cast<Uint32>(offset);
    dst.size   = static_cast<Uint32>(data.size());

    SDL_UploadToGPUBuffer(copy_pass, &src, &dst, false);
    SDL_EndGPUCopyPass(copy_pass);

    // Submit and wait
    SDL_SubmitGPUCommandBuffer(cmd);

    // Cleanup transfer buffer
    SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);

    sim_estab_log("sim_estab.gpu", severity_level::debug, "Uploaded ", data.size(), " bytes to GPU buffer");
}

std::vector<uint8_t> ComputeContext::download_buffer(ComputeBufferHandle buffer, size_t size, size_t offset) {
    check_impl();

    if (!buffer) {
        throw gpu_error("Cannot download from null buffer");
    }

    if (size == 0) {
        return {};
    }

    // Create a transfer buffer for staging
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.size  = static_cast<Uint32>(size);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;

    SDL_GPUTransferBuffer *transfer_buffer = SDL_CreateGPUTransferBuffer(impl_->gpu_device, &transfer_info);
    if (!transfer_buffer) {
        const char *error = SDL_GetError();
        throw gpu_error(std::string("Failed to create transfer buffer: ") + error);
    }

    // Create command buffer and copy pass
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu_device);
    if (!cmd) {
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to acquire command buffer");
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);
    if (!copy_pass) {
        SDL_CancelGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to begin copy pass");
    }

    SDL_GPUBufferRegion src{};
    src.buffer = static_cast<SDL_GPUBuffer *>(buffer);
    src.offset = static_cast<Uint32>(offset);
    src.size   = static_cast<Uint32>(size);

    SDL_GPUTransferBufferLocation dst{};
    dst.transfer_buffer = transfer_buffer;
    dst.offset          = 0;

    SDL_DownloadFromGPUBuffer(copy_pass, &src, &dst);
    SDL_EndGPUCopyPass(copy_pass);

    // Submit command buffer with fence for synchronization
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to submit command buffer with fence");
    }

    // Wait for completion
    SDL_WaitForGPUFences(impl_->gpu_device, true, &fence, 1);
    SDL_ReleaseGPUFence(impl_->gpu_device, fence);

    // Map and copy data
    void *mapped = SDL_MapGPUTransferBuffer(impl_->gpu_device, transfer_buffer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);
        throw gpu_error("Failed to map transfer buffer");
    }

    std::vector<uint8_t> result(size);
    std::memcpy(result.data(), mapped, size);
    SDL_UnmapGPUTransferBuffer(impl_->gpu_device, transfer_buffer);

    // Cleanup
    SDL_ReleaseGPUTransferBuffer(impl_->gpu_device, transfer_buffer);

    sim_estab_log("sim_estab.gpu", severity_level::debug, "Downloaded ", size, " bytes from GPU buffer");
    return result;
}

// Pipeline operations
ComputePipelineHandle ComputeContext::create_pipeline(std::span<const uint8_t> spirv_code, std::string_view entry_point,
                                                      uint32_t num_readonly_storage_buffers,
                                                      uint32_t num_readwrite_storage_buffers, uint32_t threadcount_x,
                                                      uint32_t threadcount_y, uint32_t threadcount_z) {
    check_impl();

    if (spirv_code.empty()) {
        throw gpu_error("Cannot create pipeline from empty shader bytecode");
    }

    // Need to keep entry_point string alive during the call
    std::string entry_point_str(entry_point);

    SDL_GPUComputePipelineCreateInfo create_info{};
    create_info.code_size                      = spirv_code.size();
    create_info.code                           = spirv_code.data();
    create_info.entrypoint                     = entry_point_str.c_str();
    create_info.format                         = SDL_GPU_SHADERFORMAT_SPIRV;
    create_info.num_readonly_storage_buffers   = num_readonly_storage_buffers;
    create_info.num_readwrite_storage_buffers  = num_readwrite_storage_buffers;
    create_info.num_uniform_buffers            = 0;
    create_info.num_readonly_storage_textures  = 0;
    create_info.num_readwrite_storage_textures = 0;
    create_info.num_samplers                   = 0;
    create_info.threadcount_x                  = threadcount_x;
    create_info.threadcount_y                  = threadcount_y;
    create_info.threadcount_z                  = threadcount_z;
    create_info.props                          = 0;

    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(impl_->gpu_device, &create_info);
    if (!pipeline) {
        const char *error = SDL_GetError();
        sim_estab_log("sim_estab.gpu", severity_level::error, "Failed to create compute pipeline: ", error);
        throw gpu_error(std::string("Failed to create compute pipeline: ") + error);
    }

    sim_estab_log("sim_estab.gpu", severity_level::debug, "Created compute pipeline with entry point: ", entry_point);
    return static_cast<ComputePipelineHandle>(pipeline);
}

void ComputeContext::destroy_pipeline(ComputePipelineHandle pipeline) {
    check_impl();
    if (pipeline) {
        SDL_ReleaseGPUComputePipeline(impl_->gpu_device, static_cast<SDL_GPUComputePipeline *>(pipeline));
        sim_estab_log("sim_estab.gpu", severity_level::debug, "Destroyed compute pipeline");
    }
}

// Dispatch operations
void ComputeContext::dispatch(ComputePipelineHandle pipeline, std::span<ComputeBufferHandle> buffers,
                              uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    check_impl();

    if (!pipeline) {
        throw gpu_error("Cannot dispatch with null pipeline");
    }

    // Acquire command buffer
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu_device);
    if (!cmd) {
        throw gpu_error("Failed to acquire command buffer for dispatch");
    }

    // Prepare storage buffer bindings
    std::vector<SDL_GPUStorageBufferReadWriteBinding> storage_bindings;
    storage_bindings.reserve(buffers.size());
    for (auto& buf : buffers) {
        SDL_GPUStorageBufferReadWriteBinding binding{};
        binding.buffer = static_cast<SDL_GPUBuffer *>(buf);
        binding.cycle  = false;
        storage_bindings.push_back(binding);
    }

    // Begin compute pass
    SDL_GPUComputePass *compute_pass =
        SDL_BeginGPUComputePass(cmd, nullptr, 0, // readonly storage textures
                                storage_bindings.data(), static_cast<Uint32>(storage_bindings.size()));

    if (!compute_pass) {
        SDL_CancelGPUCommandBuffer(cmd);
        throw gpu_error("Failed to begin compute pass");
    }

    // Bind pipeline and dispatch
    SDL_BindGPUComputePipeline(compute_pass, static_cast<SDL_GPUComputePipeline *>(pipeline));
    SDL_DispatchGPUCompute(compute_pass, group_count_x, group_count_y, group_count_z);

    SDL_EndGPUComputePass(compute_pass);

    // Submit
    SDL_SubmitGPUCommandBuffer(cmd);

    sim_estab_log("sim_estab.gpu", severity_level::debug, "Dispatched compute shader (", group_count_x, "x",
                  group_count_y, "x", group_count_z, " groups)");
}

void ComputeContext::wait_idle() {
    check_impl();
    if (impl_->gpu_device) {
        SDL_WaitForGPUIdle(impl_->gpu_device);
        sim_estab_log("sim_estab.gpu", severity_level::debug, "GPU idle");
    }
}

} // namespace sim_estab::core::gpu