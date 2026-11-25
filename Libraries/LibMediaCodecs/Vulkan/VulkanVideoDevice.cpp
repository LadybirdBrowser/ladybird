/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BuiltinWrappers.h>
#include <AK/NeverDestroyed.h>
#include <AK/OwnPtr.h>
#include <LibGfx/ImportedYCbCrVulkanImage.h>
#include <LibGfx/VulkanContext.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>

namespace Media {

namespace {

constexpr Array required_video_extensions {
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
};

constexpr Array required_export_extensions {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

constexpr Array optional_extensions {
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
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

}

Optional<VulkanVideoDevice::DecodeQueueFamily> VulkanVideoDevice::decode_queue_family_of(VkPhysicalDevice physical_device)
{
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(physical_device, &count, nullptr);
    Vector<VkQueueFamilyVideoPropertiesKHR> video_properties;
    video_properties.resize(count);
    Vector<VkQueueFamilyProperties2> properties;
    properties.resize(count);
    for (u32 index = 0; index < count; index++) {
        video_properties[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        properties[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        properties[index].pNext = &video_properties[index];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(physical_device, &count, properties.data());

    // Only one decode family is created, so it is the one that decodes the most codecs. A family may encode as well.
    constexpr VkVideoCodecOperationFlagsKHR decode_operations = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR
        | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    Optional<DecodeQueueFamily> chosen_family;
    for (u32 index = 0; index < count; index++) {
        auto const& family = properties[index].queueFamilyProperties;
        if ((family.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0 || family.queueCount == 0)
            continue;
        auto codec_operations = video_properties[index].videoCodecOperations & decode_operations;
        if (chosen_family.has_value() && popcount(codec_operations) <= popcount(chosen_family->codec_operations))
            continue;
        chosen_family = DecodeQueueFamily { index, codec_operations };
    }
    return chosen_family;
}

bool VulkanVideoDevice::supports_video_maintenance2(VkPhysicalDevice physical_device)
{
    if (!contains_extension(device_extensions_of(physical_device), VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME ""sv))
        return false;
    VkPhysicalDeviceVideoMaintenance2FeaturesKHR video_maintenance2 {};
    video_maintenance2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 features {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &video_maintenance2;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);
    return video_maintenance2.videoMaintenance2;
}

ErrorOr<void> VulkanVideoDevice::check_can_decode_video(VkPhysicalDevice physical_device)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3)
        return Error::from_string_literal("The GPU's Vulkan is older than 1.3");
    auto extensions = device_extensions_of(physical_device);
    auto has_extension = [&](auto name) { return contains_extension(extensions, { name, strlen(name) }); };
    if (!all_of(required_video_extensions, has_extension))
        return Error::from_string_literal("The GPU's Vulkan driver does not expose a video decode queue");
    if (!all_of(required_export_extensions, has_extension))
        return Error::from_string_literal("The GPU's Vulkan driver cannot export images as dma-bufs");

    // FFmpeg submits with synchronization2 and tracks its frames with timeline semaphores, without checking for either.
    VkPhysicalDeviceVulkan13Features vulkan13 {};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features vulkan12 {};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &vulkan13;
    VkPhysicalDeviceFeatures2 features {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);
    if (!vulkan12.timelineSemaphore || !vulkan13.synchronization2)
        return Error::from_string_literal("The Vulkan device lacks timeline semaphores or synchronization2");

    // FFmpeg needs a family for compute and one for transfers alongside the decoding one.
    if (!decode_queue_family_of(physical_device).has_value())
        return Error::from_string_literal("The Vulkan device lacks a decode, compute or transfer queue");
    auto queue_families = queue_families_of(physical_device);
    for (auto flags : { VK_QUEUE_COMPUTE_BIT, VK_QUEUE_TRANSFER_BIT }) {
        if (!first_queue_family_with(queue_families, flags).has_value())
            return Error::from_string_literal("The Vulkan device lacks a decode, compute or transfer queue");
    }
    return {};
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
    TRY(check_can_decode_video(device->m_physical_device));

    auto available_extensions = device_extensions_of(device->m_physical_device);
    for (auto const* name : required_video_extensions)
        device->m_enabled_extensions.append(name);
    for (auto const* name : required_export_extensions)
        device->m_enabled_extensions.append(name);

    VkPhysicalDeviceVideoMaintenance1FeaturesKHR supported_video_maintenance1 {};
    supported_video_maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
    VkPhysicalDeviceVulkan13Features supported_vulkan13 {};
    supported_vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supported_vulkan13.pNext = &supported_video_maintenance1;
    VkPhysicalDeviceVulkan12Features supported_vulkan12 {};
    supported_vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported_vulkan12.pNext = &supported_vulkan13;
    VkPhysicalDeviceVulkan11Features supported_vulkan11 {};
    supported_vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    supported_vulkan11.pNext = &supported_vulkan12;
    VkPhysicalDeviceFeatures2 supported_features {};
    supported_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_features.pNext = &supported_vulkan11;
    vkGetPhysicalDeviceFeatures2(device->m_physical_device, &supported_features);

    device->m_vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    device->m_vulkan13_features.synchronization2 = VK_TRUE;
    device->m_vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    device->m_vulkan12_features.timelineSemaphore = VK_TRUE;
    device->m_vulkan12_features.pNext = &device->m_vulkan13_features;
    device->m_vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    device->m_vulkan11_features.samplerYcbcrConversion = supported_vulkan11.samplerYcbcrConversion;
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
            if (!supports_video_maintenance2(device->m_physical_device))
                continue;
            device->m_video_maintenance2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
            device->m_video_maintenance2_features.videoMaintenance2 = VK_TRUE;
            *feature_chain_end = &device->m_video_maintenance2_features;
            feature_chain_end = &device->m_video_maintenance2_features.pNext;
        }
        device->m_enabled_extensions.append(name);
    }

    auto queue_families = queue_families_of(device->m_physical_device);
    auto decode_family = decode_queue_family_of(device->m_physical_device);
    auto compute_family = first_queue_family_with(queue_families, VK_QUEUE_COMPUTE_BIT);
    auto transfer_family = first_queue_family_with(queue_families, VK_QUEUE_TRANSFER_BIT);
    VERIFY(decode_family.has_value() && compute_family.has_value() && transfer_family.has_value());
    device->m_decode_codec_operations = decode_family->codec_operations;
    device->m_compute_queue_family = *compute_family;

    Vector<VkDeviceQueueCreateInfo> queue_infos;
    float const queue_priority = 1.0f;
    for (auto family : { decode_family->index, *compute_family, *transfer_family }) {
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
    device->m_get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device->m_logical_device, "vkGetMemoryFdKHR"));
    if (device->m_get_memory_fd == nullptr)
        return Error::from_string_literal("vkGetMemoryFdKHR is unavailable");
    device->m_get_image_drm_format_modifier_properties = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vkGetDeviceProcAddr(device->m_logical_device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (device->m_get_image_drm_format_modifier_properties == nullptr)
        return Error::from_string_literal("vkGetImageDrmFormatModifierPropertiesEXT is unavailable");

    return device;
}

VulkanVideoDevice::~VulkanVideoDevice()
{
    if (m_logical_device != VK_NULL_HANDLE)
        vkDestroyDevice(m_logical_device, nullptr);
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
}

bool VulkanVideoDevice::has_enabled_extension(StringView name) const
{
    return any_of(m_enabled_extensions, [&](auto enabled) { return StringView { enabled, strlen(enabled) } == name; });
}

Mutex& VulkanVideoDevice::queue_mutex(u32 family_index)
{
    for (size_t i = 0; i < m_queue_families.size(); i++) {
        if (m_queue_families[i].index == family_index)
            return m_queue_mutexes[i];
    }
    VERIFY_NOT_REACHED();
}

void VulkanVideoDevice::lock_queue(u32 family_index)
{
    queue_mutex(family_index).lock();
}

void VulkanVideoDevice::unlock_queue(u32 family_index)
{
    queue_mutex(family_index).unlock();
}

ErrorOr<void> VulkanVideoDevice::video_capabilities(VkVideoProfileInfoKHR const& profile, VkVideoCapabilitiesKHR& capabilities) const
{
    if (m_get_physical_device_video_capabilities(m_physical_device, &profile, &capabilities) != VK_SUCCESS)
        return Error::from_string_literal("The Vulkan device cannot decode this video profile");
    return {};
}

static Vector<VkDrmFormatModifierPropertiesEXT> drm_format_modifier_properties_of(VkPhysicalDevice physical_device, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT modifier_list {};
    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 format_properties {};
    format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_properties.pNext = &modifier_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &format_properties);

    Vector<VkDrmFormatModifierPropertiesEXT> modifier_properties;
    modifier_properties.resize(modifier_list.drmFormatModifierCount);
    modifier_list.pDrmFormatModifierProperties = modifier_properties.data();
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &format_properties);
    return modifier_properties;
}

struct DmaBufImageFormat {
    VkExtent3D maximum_extent {};
    VkExternalMemoryFeatureFlags memory_features { 0 };
};

static Optional<DmaBufImageFormat> dma_buf_image_format_of(VkPhysicalDevice physical_device, VkFormat format, u64 modifier, VkImageUsageFlags usage, VkImageCreateFlags flags, void const* video_profile_list, ReadonlySpan<u32> concurrent_queue_families)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info {};
    modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier_info.pNext = video_profile_list;
    modifier_info.drmFormatModifier = modifier;
    if (concurrent_queue_families.size() > 1) {
        modifier_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        modifier_info.queueFamilyIndexCount = concurrent_queue_families.size();
        modifier_info.pQueueFamilyIndices = concurrent_queue_families.data();
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
    if (vkGetPhysicalDeviceImageFormatProperties2(physical_device, &image_info, &image_properties) != VK_SUCCESS)
        return {};
    return DmaBufImageFormat { image_properties.imageFormatProperties.maxExtent, external_properties.externalMemoryProperties.externalMemoryFeatures };
}

Vector<u64> VulkanVideoDevice::exportable_drm_format_modifiers(VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags, void const* video_profile_list, Optional<Gfx::IntSize> size) const
{
    auto modifier_properties = drm_format_modifier_properties_of(m_physical_device, format);

    Vector<u32> queue_family_indices;
    for (auto const& family : m_queue_families)
        queue_family_indices.append(family.index);

    // An image offered several modifiers can only be as large as all of them allow.
    auto fits = [&](DmaBufImageFormat const& image_format) {
        if (!size.has_value())
            return true;
        return static_cast<u32>(size->width()) <= image_format.maximum_extent.width && static_cast<u32>(size->height()) <= image_format.maximum_extent.height;
    };

    Vector<u64> modifiers;
    for (auto const& properties : modifier_properties) {
        // The Compositor runs on the same device and driver, so it sees the same features under each modifier.
        auto required_features = Gfx::ImportedYCbCrVulkanImage::REQUIRED_FORMAT_FEATURES;
        if ((properties.drmFormatModifierTilingFeatures & required_features) != required_features)
            continue;
        if (properties.drmFormatModifierPlaneCount > Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
            continue;

        auto exported = dma_buf_image_format_of(m_physical_device, format, properties.drmFormatModifier, usage, flags, video_profile_list, queue_family_indices);
        if (!exported.has_value() || (exported->memory_features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0 || !fits(*exported))
            continue;

        // The Compositor imports the image under usage of its own, which the memory features may differ under.
        auto imported = dma_buf_image_format_of(m_physical_device, format, properties.drmFormatModifier, Gfx::ImportedYCbCrVulkanImage::USAGE, 0, nullptr, {});
        if (!imported.has_value() || (imported->memory_features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0 || !fits(*imported))
            continue;

        modifiers.append(properties.drmFormatModifier);
    }
    return modifiers;
}

ErrorOr<Gfx::LinuxDmaBufHandle> VulkanVideoDevice::export_image_as_dma_buf(VkImage image, VkDeviceMemory memory, VkFormat format, Gfx::IntSize size, u32 drm_format) const
{
    // The driver picks one of the modifiers that the image was offered when it was created.
    VkImageDrmFormatModifierPropertiesEXT image_modifier {};
    image_modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (m_get_image_drm_format_modifier_properties(m_logical_device, image, &image_modifier) != VK_SUCCESS)
        return Error::from_string_literal("Could not find the DRM format modifier of an image");

    Optional<u32> plane_count;
    for (auto const& properties : drm_format_modifier_properties_of(m_physical_device, format)) {
        if (properties.drmFormatModifier == image_modifier.drmFormatModifier)
            plane_count = properties.drmFormatModifierPlaneCount;
    }
    if (!plane_count.has_value() || *plane_count < 1 || *plane_count > Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
        return Error::from_string_literal("An image's DRM format modifier lays it out in an unusable number of planes");

    VkMemoryGetFdInfoKHR fd_info {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (m_get_memory_fd(m_logical_device, &fd_info, &fd) != VK_SUCCESS)
        return Error::from_string_literal("Could not export an image's memory as a dma-buf");

    Gfx::LinuxDmaBufHandle dma_buf;
    dma_buf.size = size;
    dma_buf.drm_format = drm_format;
    dma_buf.modifier = image_modifier.drmFormatModifier;
    dma_buf.objects.append(IPC::File::adopt_fd(fd));

    static constexpr Array memory_plane_aspects {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
    };
    for (u32 plane_index = 0; plane_index < *plane_count; plane_index++) {
        VkImageSubresource subresource {};
        subresource.aspectMask = memory_plane_aspects[plane_index];
        VkSubresourceLayout layout {};
        vkGetImageSubresourceLayout(m_logical_device, image, &subresource, &layout);
        dma_buf.planes.append({ 0, static_cast<u32>(layout.offset), static_cast<u32>(layout.rowPitch) });
    }
    return dma_buf;
}

}
