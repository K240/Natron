// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_VULKAN_TEST_SUITE_H
#define NATRON_OFX_VULKAN_TEST_SUITE_H
#include "Global/OpenFX/NatronVulkanImageSuite.h"
#define kNatronOfxVulkanTestSuite "NatronOfxVulkanTestSuite"
// Only exposed with NATRON_TEST_PROFILE. These deterministic result faults do
// not reset the physical driver. Production plugins must not use this suite.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NatronOfxVulkanTestSuiteV1 {
    OfxStatus (*getDeviceGeneration)(NatronVulkanContextHandle context, uint64_t* generation);
    // point: 1 = before submit, 2 = after completion, 3 = before present.
    OfxStatus (*armDeviceLoss)(NatronVulkanContextHandle context, uint32_t point);
} NatronOfxVulkanTestSuiteV1;
typedef struct NatronVulkanTestStatistics {
    uint64_t generation;
    uint64_t imageUploads, imageDownloads, imageUploadBytes, imageDownloadBytes;
    // Callback-tracked VMA blocks on device-local heaps, including staging on
    // unified memory. The peak is for this device's entire lifetime. Swapchain
    // and driver-owned allocations are outside VMA and outside this peak.
    uint64_t managedLocalBlockBytes, managedLocalPeakBlockBytes, managedLocalAllocationBytes;
    // Instantaneous VK_EXT_memory_budget samples. These are not a driver VRAM
    // peak. Usage estimates this process; budget changes with system activity.
    // Both fields are zero when the extension is absent.
    uint64_t driverLocalUsageBytes, driverLocalBudgetBytes;
    uint32_t driverBudgetAvailable, validationErrors;
} NatronVulkanTestStatistics;
// V1 remains available with its original layout; V2 preserves its prefix.
typedef struct NatronOfxVulkanTestSuiteV2 {
    OfxStatus (*getDeviceGeneration)(NatronVulkanContextHandle context, uint64_t* generation);
    OfxStatus (*armDeviceLoss)(NatronVulkanContextHandle context, uint32_t point);
    // Compare related counters at quiescent points; individual fields are
    // atomic, but the entire snapshot is not one transaction with render work.
    OfxStatus (*getStatistics)(NatronVulkanContextHandle context, NatronVulkanTestStatistics* statistics);
    // Quiescent, profile-owned benchmarks only. Purges Engine image entries
    // outside the timed interval while retaining compiled plugin programs.
    OfxStatus (*clearNodeCache)(void);
} NatronOfxVulkanTestSuiteV2;
#ifdef __cplusplus
}
#endif
#endif
