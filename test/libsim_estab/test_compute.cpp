/**
 * @file test_compute.cpp
 * @brief Tests for ComputeContext - headless GPU compute
 *
 * These tests verify the headless GPU compute functionality.
 * Unlike viz tests, these do NOT require a display environment.
 * Without one, SDL falls back to its offscreen driver (default set in test_main.cpp).
 */

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// compat headers
#include <compat.h>

// import modules
import sim_estab;

namespace utf = boost::unit_test;
using namespace sim_estab::core::gpu;

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(compute_tests)

/**
 * @brief Test ComputeContext default construction
 */
BOOST_AUTO_TEST_CASE(test_compute_context_creation,
                     *utf::description("Test ComputeContext creation with default parameters")) {
    BOOST_TEST_MESSAGE("Creating ComputeContext with default parameters");

    ComputeContext ctx;

    BOOST_TEST(ctx.has_device());

    auto info = ctx.device_info();
    BOOST_TEST_MESSAGE("GPU Backend: " << static_cast<int>(info.backend));
    BOOST_TEST_MESSAGE("GPU Driver: " << info.driver_name);

    // Should have a valid backend (Vulkan or D3D12 on most systems)
    BOOST_TEST((info.backend == GPUBackend::Vulkan || info.backend == GPUBackend::D3D12 ||
                info.backend == GPUBackend::Unknown));
}

/**
 * @brief Test ComputeContext with performance/default GPU preference
 */
BOOST_AUTO_TEST_CASE(test_compute_context_prefer_performance,
                     *utf::description("Test ComputeContext creation with performance/default GPU preference")) {
    BOOST_TEST_MESSAGE("Creating ComputeContext preferring performance/default GPU selection");

    ComputeContext ctx(false);

    BOOST_TEST(ctx.has_device());
}

/**
 * @brief Test ComputeContext with low-power GPU preference
 */
BOOST_AUTO_TEST_CASE(test_compute_context_prefer_low_power,
                     *utf::description("Test ComputeContext creation with low-power GPU preference")) {
    BOOST_TEST_MESSAGE("Creating ComputeContext preferring low-power GPU");

    ComputeContext ctx(true);

    BOOST_TEST(ctx.has_device());
}

/**
 * @brief Test mixed GPU preference requests while sharing the device
 */
BOOST_AUTO_TEST_CASE(test_compute_context_mixed_device_preferences,
                     *utf::description("Test multiple ComputeContext instances with different GPU preferences")) {
    BOOST_TEST_MESSAGE("Testing mixed GPU preference requests");

    ComputeContext ctx1(false);
    BOOST_TEST(ctx1.has_device());

    ComputeContext ctx2(true);
    BOOST_TEST(ctx2.has_device());

    auto info1 = ctx1.device_info();
    auto info2 = ctx2.device_info();
    BOOST_TEST(static_cast<int>(info1.backend) == static_cast<int>(info2.backend));
}

/**
 * @brief Test ComputeContext buffer creation and destruction
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_lifecycle, *utf::description("Test GPU buffer create/destroy")) {
    BOOST_TEST_MESSAGE("Testing buffer lifecycle operations");

    ComputeContext ctx;

    // Create a buffer
    const size_t buffer_size = 1024;
    auto buffer =
        ctx.create_buffer(buffer_size, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    BOOST_TEST(buffer != nullptr);

    // Destroy the buffer
    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("Buffer lifecycle test completed");
}

/**
 * @brief Test ComputeContext buffer upload/download
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_transfer, *utf::description("Test GPU buffer data transfer")) {
    BOOST_TEST_MESSAGE("Testing buffer upload and download");

    ComputeContext ctx;

    // Create test data
    std::vector<uint8_t> upload_data(256);
    for (size_t i = 0; i < upload_data.size(); ++i) {
        upload_data[i] = static_cast<uint8_t>(i);
    }

    // Create buffer
    auto buffer = ctx.create_buffer(upload_data.size(),
                                    BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    // Upload data
    ctx.upload_buffer(buffer, std::span<const uint8_t>(upload_data));

    // Download data
    auto download_data = ctx.download_buffer(buffer, upload_data.size());

    // Verify data
    BOOST_TEST(download_data.size() == upload_data.size());
    BOOST_TEST(std::equal(upload_data.begin(), upload_data.end(), download_data.begin()));

    // Cleanup
    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("Buffer transfer test completed");
}

/**
 * @brief Test ComputeContext buffer partial upload/download
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_partial_transfer,
                     *utf::description("Test GPU buffer partial data transfer with offsets")) {
    BOOST_TEST_MESSAGE("Testing buffer partial upload and download");

    ComputeContext ctx;

    const size_t buffer_size = 1024;
    const size_t offset      = 256;
    const size_t data_size   = 128;

    // Create buffer
    auto buffer =
        ctx.create_buffer(buffer_size, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    // Create test data
    std::vector<uint8_t> upload_data(data_size);
    for (size_t i = 0; i < upload_data.size(); ++i) {
        upload_data[i] = static_cast<uint8_t>(i + 128);
    }

    // Upload data at offset
    ctx.upload_buffer(buffer, std::span<const uint8_t>(upload_data), offset);

    // Download data from offset
    auto download_data = ctx.download_buffer(buffer, data_size, offset);

    // Verify data
    BOOST_TEST(download_data.size() == upload_data.size());
    BOOST_TEST(std::equal(upload_data.begin(), upload_data.end(), download_data.begin()));

    // Cleanup
    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("Partial buffer transfer test completed");
}

/**
 * @brief An offset at 4 GiB or past it throws, and the buffer keeps its data
 *
 * An SDL GPU buffer region has a 32-bit offset, so no buffer has a byte at 4 GiB or past
 * it. The call validates its arguments up front and throws for such an offset
 * (design_patterns.md §6). It must not cut the offset to 32 bits and use the wrapped
 * offset, here 0.
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_offset_past_4_gib_throws,
                     *utf::description("Test that a GPU buffer offset at 4 GiB or past it throws")) {
    BOOST_TEST_MESSAGE("Testing buffer transfers at an offset past 4 GiB");

    ComputeContext ctx;

    const size_t data_size = 256;
    const size_t past      = size_t{1} << 32; // 0 when cut to 32 bits

    auto buffer =
        ctx.create_buffer(data_size, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    std::vector<uint8_t> kept(data_size);
    std::vector<uint8_t> other(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        kept[i]  = static_cast<uint8_t>(i);
        other[i] = static_cast<uint8_t>(255 - i);
    }
    ctx.upload_buffer(buffer, std::span<const uint8_t>(kept));

    BOOST_CHECK_THROW(ctx.upload_buffer(buffer, std::span<const uint8_t>(other), past), gpu_error);
    BOOST_CHECK_THROW((void)ctx.download_buffer(buffer, data_size, past), gpu_error);

    // The refused upload wrote nothing
    auto download_data = ctx.download_buffer(buffer, data_size);
    BOOST_TEST(download_data.size() == kept.size());
    BOOST_TEST(std::equal(kept.begin(), kept.end(), download_data.begin()));

    // Cleanup
    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("Offset past 4 GiB test completed");
}

/**
 * @brief A buffer size at 4 GiB or past it throws
 *
 * An SDL GPU buffer has a 32-bit size, so no buffer holds 4 GiB or more. The call
 * validates its arguments up front and throws for such a size (design_patterns.md §6).
 * It must not cut the size to 32 bits and create a buffer of the wrapped size, here 16
 * bytes.
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_size_past_4_gib_throws,
                     *utf::description("Test that a GPU buffer size at 4 GiB or past it throws")) {
    BOOST_TEST_MESSAGE("Testing buffer creation with a size past 4 GiB");

    ComputeContext ctx;

    const size_t past = (size_t{1} << 32) + 16; // 16 when cut to 32 bits

    ComputeBufferHandle created = nullptr;
    BOOST_CHECK_THROW(created = ctx.create_buffer(past, BufferUsage::Storage), gpu_error);
    if (created) {
        ctx.destroy_buffer(created); // the wrapped buffer a failing implementation made
    }

    BOOST_TEST_MESSAGE("Buffer size past 4 GiB test completed");
}

/**
 * @brief A transfer at an offset lands at that offset, and touches nothing else
 *
 * The upload writes its data at the offset and leaves the rest of the buffer as it was,
 * and the download reads from the offset. A round trip at one offset passes even when
 * both transfers ignore it, so this case reads the whole buffer back.
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_transfer_lands_at_the_offset,
                     *utf::description("Test that a GPU buffer transfer at an offset lands at that offset")) {
    BOOST_TEST_MESSAGE("Testing that buffer transfers land at their offset");

    ComputeContext ctx;

    const size_t buffer_size = 1024;
    const size_t offset      = 256;
    const size_t patch_size  = 128;

    auto buffer =
        ctx.create_buffer(buffer_size, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    // Fill the whole buffer, then patch a window in the middle with different bytes
    std::vector<uint8_t> whole(buffer_size);
    std::vector<uint8_t> patch(patch_size);
    for (size_t i = 0; i < buffer_size; ++i) {
        whole[i] = static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < patch_size; ++i) {
        patch[i] = static_cast<uint8_t>(255 - i); // differs from whole[offset + i], which is i
    }
    ctx.upload_buffer(buffer, std::span<const uint8_t>(whole));
    ctx.upload_buffer(buffer, std::span<const uint8_t>(patch), offset);

    // The whole buffer: the patch at the offset, the old bytes elsewhere
    std::vector<uint8_t> expected = whole;
    std::copy(patch.begin(), patch.end(), expected.begin() + static_cast<std::ptrdiff_t>(offset));
    auto download_whole = ctx.download_buffer(buffer, buffer_size);
    BOOST_TEST(download_whole.size() == expected.size());
    BOOST_TEST(std::equal(expected.begin(), expected.end(), download_whole.begin()));

    // A download at the offset reads the patch, not the start of the buffer
    auto download_patch = ctx.download_buffer(buffer, patch_size, offset);
    BOOST_TEST(download_patch.size() == patch.size());
    BOOST_TEST(std::equal(patch.begin(), patch.end(), download_patch.begin()));

    // Cleanup
    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("Transfer at an offset test completed");
}

/**
 * @brief Test multiple ComputeContext instances
 */
BOOST_AUTO_TEST_CASE(test_compute_multiple_contexts,
                     *utf::description("Test multiple ComputeContext instances sharing GPU device")) {
    BOOST_TEST_MESSAGE("Testing multiple ComputeContext instances");

    // Create first context
    ComputeContext ctx1;
    BOOST_TEST(ctx1.has_device());

    auto info1 = ctx1.device_info();

    // Create second context
    ComputeContext ctx2;
    BOOST_TEST(ctx2.has_device());

    auto info2 = ctx2.device_info();

    // Both contexts should share the same device (same backend)
    BOOST_TEST(static_cast<int>(info1.backend) == static_cast<int>(info2.backend));

    BOOST_TEST_MESSAGE("Multiple contexts test completed");
}

/**
 * @brief Test ComputeContext buffer usage flags
 */
BOOST_AUTO_TEST_CASE(test_compute_buffer_usage_flags,
                     *utf::description("Test various buffer usage flag combinations")) {
    BOOST_TEST_MESSAGE("Testing various buffer usage flags");

    ComputeContext ctx;

    // Test different usage combinations
    struct TestCase {
        BufferUsage usage;
        const char *description;
    };

    std::array<TestCase, 5> test_cases = {{
        {BufferUsage::Storage | BufferUsage::TransferDst, "Storage + TransferDst"},
        {BufferUsage::Storage | BufferUsage::TransferSrc, "Storage + TransferSrc"},
        {BufferUsage::Storage | BufferUsage::ComputeRead, "Storage + ComputeRead"},
        {BufferUsage::Storage | BufferUsage::ComputeWrite, "Storage + ComputeWrite"},
        {BufferUsage::Storage | BufferUsage::ComputeRead | BufferUsage::ComputeWrite | BufferUsage::TransferDst |
             BufferUsage::TransferSrc,
         "All flags"},
    }};

    for (const auto& test_case : test_cases) {
        BOOST_TEST_MESSAGE("  Testing: " << test_case.description);

        auto buffer = ctx.create_buffer(256, test_case.usage);
        BOOST_TEST(buffer != nullptr);
        ctx.destroy_buffer(buffer);
    }

    BOOST_TEST_MESSAGE("Buffer usage flags test completed");
}

/**
 * @brief Test ComputeContext wait_idle
 */
BOOST_AUTO_TEST_CASE(test_compute_wait_idle, *utf::description("Test GPU synchronization with wait_idle")) {
    BOOST_TEST_MESSAGE("Testing wait_idle synchronization");

    ComputeContext ctx;

    // Create and upload buffer
    std::vector<uint8_t> data(128, 0x42);
    auto buffer =
        ctx.create_buffer(data.size(), BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);

    ctx.upload_buffer(buffer, std::span<const uint8_t>(data));

    // Wait for GPU to finish
    ctx.wait_idle();

    // Should be safe to download now
    auto download_data = ctx.download_buffer(buffer, data.size());
    BOOST_TEST(download_data.size() == data.size());

    ctx.destroy_buffer(buffer);

    BOOST_TEST_MESSAGE("wait_idle test completed");
}

/**
 * @brief Test BufferUsage bitwise operators
 */
BOOST_AUTO_TEST_CASE(test_buffer_usage_operators, *utf::description("Test BufferUsage enum bitwise operators")) {
    BOOST_TEST_MESSAGE("Testing BufferUsage bitwise operators");

    // Test OR operator
    auto usage1 = BufferUsage::Storage | BufferUsage::TransferDst;
    BOOST_TEST(has_flag(usage1, BufferUsage::Storage));
    BOOST_TEST(has_flag(usage1, BufferUsage::TransferDst));
    BOOST_TEST(!has_flag(usage1, BufferUsage::TransferSrc));

    // Test AND operator
    auto usage2 = usage1 & BufferUsage::Storage;
    BOOST_TEST(has_flag(usage2, BufferUsage::Storage));
    BOOST_TEST(!has_flag(usage2, BufferUsage::TransferDst));

    // Test has_flag
    auto usage3 = BufferUsage::ComputeRead | BufferUsage::ComputeWrite;
    BOOST_TEST(has_flag(usage3, BufferUsage::ComputeRead));
    BOOST_TEST(has_flag(usage3, BufferUsage::ComputeWrite));
    BOOST_TEST(!has_flag(usage3, BufferUsage::Storage));

    BOOST_TEST_MESSAGE("BufferUsage operators test completed");
}

BOOST_AUTO_TEST_SUITE_END()
