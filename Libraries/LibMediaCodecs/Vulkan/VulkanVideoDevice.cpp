/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NeverDestroyed.h>
#include <AK/OwnPtr.h>
#include <LibGfx/ImportedYCbCrVulkanImage.h>
#include <LibGfx/VulkanContext.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>

namespace Media {

namespace {

constexpr Array required_extensions {
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

constexpr Array optional_extensions {
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
    VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
    VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME,
};

NeverDestroyed<OwnPtr<VulkanVideoDevice>> s_process_device;

Vector<VkExtensionProperties> device_extensions_of(VkPhysicalDevice physical_device)
{
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    Vector<VkExtensionProperties> extensions;
    extensions.resize(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    return extensions;
}

bool contains_extension(ReadonlySpan<VkExtensionProperties> extensions, StringView name)
{
    return any_of(extensions, [&](auto const& extension) { return StringView { extension.extensionName, strlen(extension.extensionName) } == name; });
}

Vector<VkQueueFamilyProperties> queue_families_of(VkPhysicalDevice physical_device)
{
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    Vector<VkQueueFamilyProperties> families;
    families.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
    return families;
}

Optional<u32> first_queue_family_with(ReadonlySpan<VkQueueFamilyProperties> families, VkQueueFlags flags)
{
    for (u32 index = 0; index < families.size(); index++) {
        if ((families[index].queueFlags & flags) == flags && families[index].queueCount > 0)
            return index;
    }
    return {};
}

bool can_decode_video(VkPhysicalDevice physical_device)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3)
        return false;
    auto extensions = device_extensions_of(physical_device);
    if (!all_of(required_extensions, [&](auto name) { return contains_extension(extensions, { name, strlen(name) }); }))
        return false;
    return first_queue_family_with(queue_families_of(physical_device), VK_QUEUE_VIDEO_DECODE_BIT_KHR).has_value();
}

}

ErrorOr<void> VulkanVideoDevice::create_for_process(Gfx::GpuDeviceIdentity const& identity)
{
    VERIFY(!*s_process_device);
    *s_process_device = TRY(create(identity));
    return {};
}

VulkanVideoDevice* VulkanVideoDevice::the()
{
    return s_process_device->ptr();
}

ErrorOr<NonnullOwnPtr<VulkanVideoDevice>> VulkanVideoDevice::create(Gfx::GpuDeviceIdentity const& identity)
{
    auto device = adopt_own(*new VulkanVideoDevice);
    device->m_api_version = VK_API_VERSION_1_3;

    VkApplicationInfo application_info {};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Ladybird";
    application_info.apiVersion = device->m_api_version;

    VkInstanceCreateInfo instance_info {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    if (vkCreateInstance(&instance_info, nullptr, &device->m_instance) != VK_SUCCESS)
        return Error::from_string_literal("Could not create a Vulkan instance");

    u32 physical_device_count = 0;
    vkEnumeratePhysicalDevices(device->m_instance, &physical_device_count, nullptr);
    Vector<VkPhysicalDevice> physical_devices;
    physical_devices.resize(physical_device_count);
    vkEnumeratePhysicalDevices(device->m_instance, &physical_device_count, physical_devices.data());

    for (auto* physical_device : physical_devices) {
        if (Gfx::gpu_device_identity_of(physical_device) == identity) {
            device->m_physical_device = physical_device;
            break;
        }
    }
    if (device->m_physical_device == VK_NULL_HANDLE)
        return Error::from_string_literal("The named GPU is not a Vulkan device here");
    if (!can_decode_video(device->m_physical_device))
        return Error::from_string_literal("The named GPU cannot decode video into images that export as dma-bufs");

    auto available_extensions = device_extensions_of(device->m_physical_device);
    for (auto const* name : required_extensions)
        device->m_enabled_extensions.append(name);

    VkPhysicalDeviceVideoMaintenance2FeaturesKHR supported_video_maintenance2 {};
    supported_video_maintenance2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR supported_video_maintenance1 {};
    supported_video_maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
    supported_video_maintenance1.pNext = &supported_video_maintenance2;
    VkPhysicalDeviceVulkan13Features supported_vulkan13 {};
    supported_vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supported_vulkan13.pNext = &supported_video_maintenance1;
    VkPhysicalDeviceVulkan12Features supported_vulkan12 {};
    supported_vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported_vulkan12.pNext = &supported_vulkan13;
    VkPhysicalDeviceFeatures2 supported_features {};
    supported_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_features.pNext = &supported_vulkan12;
    vkGetPhysicalDeviceFeatures2(device->m_physical_device, &supported_features);

    // FFmpeg submits with synchronization2 and tracks its frames with timeline semaphores, without checking for either.
    if (!supported_vulkan12.timelineSemaphore || !supported_vulkan13.synchronization2)
        return Error::from_string_literal("The Vulkan device lacks timeline semaphores or synchronization2");

    device->m_vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    device->m_vulkan13_features.synchronization2 = VK_TRUE;
    device->m_vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    device->m_vulkan12_features.timelineSemaphore = VK_TRUE;
    device->m_vulkan12_features.pNext = &device->m_vulkan13_features;
    device->m_vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    device->m_vulkan11_features.pNext = &device->m_vulkan12_features;
    device->m_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device->m_features.pNext = &device->m_vulkan11_features;
    void** feature_chain_end = &device->m_vulkan13_features.pNext;

    // FFmpeg also trusts the extension's name alone, so an extension whose feature is missing stays disabled.
    for (auto const* name : optional_extensions) {
        StringView name_view { name, strlen(name) };
        if (!contains_extension(available_extensions, name_view))
            continue;
        if (name_view == VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME ""sv) {
            if (!supported_video_maintenance1.videoMaintenance1)
                continue;
            device->m_video_maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
            device->m_video_maintenance1_features.videoMaintenance1 = VK_TRUE;
            *feature_chain_end = &device->m_video_maintenance1_features;
            feature_chain_end = &device->m_video_maintenance1_features.pNext;
        } else if (name_view == VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME ""sv) {
            if (!supported_video_maintenance2.videoMaintenance2)
                continue;
            device->m_video_maintenance2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
            device->m_video_maintenance2_features.videoMaintenance2 = VK_TRUE;
            *feature_chain_end = &device->m_video_maintenance2_features;
            feature_chain_end = &device->m_video_maintenance2_features.pNext;
        }
        device->m_enabled_extensions.append(name);
    }

    // FFmpeg needs a family for compute and one for transfers alongside the decoding one.
    auto queue_families = queue_families_of(device->m_physical_device);
    auto decode_family = first_queue_family_with(queue_families, VK_QUEUE_VIDEO_DECODE_BIT_KHR);
    auto compute_family = first_queue_family_with(queue_families, VK_QUEUE_COMPUTE_BIT);
    auto transfer_family = first_queue_family_with(queue_families, VK_QUEUE_TRANSFER_BIT);
    if (!decode_family.has_value() || !compute_family.has_value() || !transfer_family.has_value())
        return Error::from_string_literal("The Vulkan device lacks a decode, compute or transfer queue");

    Vector<VkDeviceQueueCreateInfo> queue_infos;
    float const queue_priority = 1.0f;
    for (auto family : { *decode_family, *compute_family, *transfer_family }) {
        if (any_of(device->m_queue_families, [&](auto const& existing) { return existing.index == family; }))
            continue;
        device->m_queue_families.append({ family, queue_families[family].queueFlags });
        VkDeviceQueueCreateInfo queue_info {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        queue_infos.append(queue_info);
    }

    VkDeviceCreateInfo device_info {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &device->m_features;
    device_info.queueCreateInfoCount = queue_infos.size();
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = device->m_enabled_extensions.size();
    device_info.ppEnabledExtensionNames = device->m_enabled_extensions.data();
    if (vkCreateDevice(device->m_physical_device, &device_info, nullptr, &device->m_logical_device) != VK_SUCCESS)
        return Error::from_string_literal("Could not create a Vulkan device for decoding video");

    device->m_get_physical_device_video_capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(vkGetInstanceProcAddr(device->m_instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if (device->m_get_physical_device_video_capabilities == nullptr)
        return Error::from_string_literal("vkGetPhysicalDeviceVideoCapabilitiesKHR is unavailable");

    return device;
}

VulkanVideoDevice::~VulkanVideoDevice()
{
    if (m_logical_device != VK_NULL_HANDLE)
        vkDestroyDevice(m_logical_device, nullptr);
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
}

ErrorOr<void> VulkanVideoDevice::video_capabilities(VkVideoProfileInfoKHR const& profile, VkVideoCapabilitiesKHR& capabilities) const
{
    if (m_get_physical_device_video_capabilities(m_physical_device, &profile, &capabilities) != VK_SUCCESS)
        return Error::from_string_literal("The Vulkan device cannot decode this video profile");
    return {};
}

Vector<u64> VulkanVideoDevice::exportable_drm_format_modifiers(VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags, void const* video_profile_list) const
{
    VkDrmFormatModifierPropertiesListEXT modifier_list {};
    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 format_properties {};
    format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_properties.pNext = &modifier_list;
    vkGetPhysicalDeviceFormatProperties2(m_physical_device, format, &format_properties);

    Vector<VkDrmFormatModifierPropertiesEXT> modifier_properties;
    modifier_properties.resize(modifier_list.drmFormatModifierCount);
    modifier_list.pDrmFormatModifierProperties = modifier_properties.data();
    vkGetPhysicalDeviceFormatProperties2(m_physical_device, format, &format_properties);

    Vector<u32> queue_family_indices;
    for (auto const& family : m_queue_families)
        queue_family_indices.append(family.index);

    Vector<u64> modifiers;
    for (auto const& properties : modifier_properties) {
        // The Compositor runs on the same device and driver, so it sees the same features under each modifier.
        auto required_features = Gfx::ImportedYCbCrVulkanImage::REQUIRED_FORMAT_FEATURES;
        if ((properties.drmFormatModifierTilingFeatures & required_features) != required_features)
            continue;

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info {};
        modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modifier_info.pNext = video_profile_list;
        modifier_info.drmFormatModifier = properties.drmFormatModifier;
        if (queue_family_indices.size() > 1) {
            modifier_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            modifier_info.queueFamilyIndexCount = queue_family_indices.size();
            modifier_info.pQueueFamilyIndices = queue_family_indices.data();
        }

        VkPhysicalDeviceExternalImageFormatInfo external_info {};
        external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external_info.pNext = &modifier_info;
        external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageFormatInfo2 image_info {};
        image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        image_info.pNext = &external_info;
        image_info.format = format;
        image_info.type = VK_IMAGE_TYPE_2D;
        image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = usage;
        image_info.flags = flags;

        VkExternalImageFormatProperties external_properties {};
        external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 image_properties {};
        image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        image_properties.pNext = &external_properties;
        if (vkGetPhysicalDeviceImageFormatProperties2(m_physical_device, &image_info, &image_properties) != VK_SUCCESS)
            continue;
        if ((external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0)
            continue;
        modifiers.append(properties.drmFormatModifier);
    }
    return modifiers;
}

}
