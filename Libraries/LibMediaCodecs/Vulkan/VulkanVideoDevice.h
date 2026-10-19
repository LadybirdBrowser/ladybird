/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Array.h>
#    include <AK/Error.h>
#    include <AK/Mutex.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/Vector.h>
#    include <LibGfx/GpuDeviceIdentity.h>
#    include <LibMediaCodecs/Export.h>
#    include <vulkan/vulkan.h>

namespace Media {

// A Vulkan device for decoding video into images that export as dma-bufs. One serves every decoder in a process, and
// outlives every frame decoded on it.
class MEDIACODECS_API VulkanVideoDevice {
    AK_MAKE_NONCOPYABLE(VulkanVideoDevice);
    AK_MAKE_NONMOVABLE(VulkanVideoDevice);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct QueueFamily {
        u32 index { 0 };
        VkQueueFlags flags { 0 };
    };

    // Decoding happens on the GPU that will display the frames, so it is named by the process that displays them. The
    // device is created before the process starts any other threads, and is never destroyed.
    static ErrorOr<void> create_for_process(Gfx::GpuDeviceIdentity const&);
    static VulkanVideoDevice* the();

    ~VulkanVideoDevice();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }
    VkDevice logical_device() const { return m_logical_device; }
    u32 api_version() const { return m_api_version; }
    ReadonlySpan<QueueFamily> queue_families() const { return m_queue_families; }
    ReadonlySpan<char const*> enabled_extensions() const { return m_enabled_extensions; }
    VkPhysicalDeviceFeatures2 const& enabled_features() const { return m_features; }
    bool has_enabled_extension(StringView) const;

    // A queue takes one submission at a time, and every FFmpeg in the process submits to this device's queues. Each
    // family has one queue.
    void lock_queue(u32 family_index);
    void unlock_queue(u32 family_index);

    // Fills in the capabilities, and whatever codec-specific capabilities are chained to them, for a video profile.
    ErrorOr<void> video_capabilities(VkVideoProfileInfoKHR const&, VkVideoCapabilitiesKHR&) const;

    // The DRM format modifiers that images of the format can be created and exported under, among those that the
    // Compositor can import images with.
    Vector<u64> exportable_drm_format_modifiers(VkFormat, VkImageUsageFlags, VkImageCreateFlags, void const* video_profile_list) const;

private:
    VulkanVideoDevice() = default;

    static ErrorOr<NonnullOwnPtr<VulkanVideoDevice>> create(Gfx::GpuDeviceIdentity const&);

    Mutex& queue_mutex(u32 family_index);

    u32 m_api_version { 0 };
    VkInstance m_instance { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice m_logical_device { VK_NULL_HANDLE };
    Vector<QueueFamily> m_queue_families;
    // One for each of the decode, compute and transfer families, which may coincide.
    Array<Mutex, 3> m_queue_mutexes;
    Vector<char const*> m_enabled_extensions;

    VkPhysicalDeviceFeatures2 m_features {};
    VkPhysicalDeviceVulkan11Features m_vulkan11_features {};
    VkPhysicalDeviceVulkan12Features m_vulkan12_features {};
    VkPhysicalDeviceVulkan13Features m_vulkan13_features {};
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR m_video_maintenance1_features {};
    VkPhysicalDeviceVideoMaintenance2FeaturesKHR m_video_maintenance2_features {};

    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR m_get_physical_device_video_capabilities { nullptr };
};

}

#endif
