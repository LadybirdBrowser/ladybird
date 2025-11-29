/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/VulkanContext.h>

namespace Gfx {

ErrorOr<NonnullRefPtr<VulkanContext>> VulkanContext::create()
{
    auto context = adopt_ref(*new VulkanContext());

    TRY(context->create_instance());
    TRY(context->pick_physical_device());
    TRY(context->create_logical_device_and_queue());

#ifdef USE_VULKAN_IMAGES
    TRY(context->create_command_pool());
    TRY(context->allocate_command_buffer());
    TRY(context->get_extensions());
#endif

    return context;
}

VulkanContext::VulkanContext()
{
    // NOTE: v1.1 needed for vkGetPhysicalDeviceFormatProperties2
    // TODO: Try v1.0 if v1.1 is not supported as it's only needed for Vulkan images
    m_api_version = VK_API_VERSION_1_1;
}

VulkanContext::~VulkanContext()
{
    if (m_logical_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_logical_device, nullptr);
    }

#if VULKAN_DEBUG
    if (m_debug_messenger != VK_NULL_HANDLE) {
        auto pfn_destroy_debug_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        pfn_destroy_debug_messenger(m_instance, m_debug_messenger, nullptr);
    }
#endif

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

ErrorOr<void> VulkanContext::create_instance()
{
    VkApplicationInfo app_info {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Ladybird";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = nullptr;
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = m_api_version;

#if VULKAN_DEBUG
    Array<char const*, 1> layers = { "VK_LAYER_KHRONOS_validation" };
    Array<char const*, 1> extensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    bool validation_layer_available = check_layer_support("VK_LAYER_KHRONOS_validation"sv);
    if (!validation_layer_available) {
        dbgln("Requested Vulkan validation layer not available");
    }
#endif

    VkInstanceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
#if VULKAN_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info {};
    debug_messenger_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_messenger_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_messenger_create_info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData,
                                                      void*) {
        dbgln("Vulkan validation layer: {}", pCallbackData->pMessage);
        return VK_FALSE;
    };

    if (validation_layer_available) {
        create_info.enabledLayerCount = layers.size();
        create_info.ppEnabledLayerNames = layers.data();
        create_info.enabledExtensionCount = extensions.size();
        create_info.ppEnabledExtensionNames = extensions.data();
        create_info.pNext = &debug_messenger_create_info;
    }
#endif

    auto result = vkCreateInstance(&create_info, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateInstance returned {}", to_underlying(result));
        return Error::from_string_literal("Application instance creation failed");
    }

#if VULKAN_DEBUG
    if (validation_layer_available) {
        auto pfn_create_debug_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        result = pfn_create_debug_messenger(m_instance, &debug_messenger_create_info, nullptr, &m_debug_messenger);
        if (result != VK_SUCCESS) {
            dbgln("vkCreateDebugUtilsMessengerEXT returned {}", to_underlying(result));
        }
    }
#endif

    return {};
}

ErrorOr<void> VulkanContext::pick_physical_device()
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);

    if (device_count == 0)
        return Error::from_string_literal("Can't find any physical devices available");

    Vector<VkPhysicalDevice> devices;
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());

    // Pick discrete GPU or the first device in the list
    for (auto const& device : devices) {
        if (m_physical_device == VK_NULL_HANDLE)
            m_physical_device = device;

        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);
        if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            m_physical_device = device;
    }

    VERIFY(m_physical_device != VK_NULL_HANDLE);
    return {};
}

ErrorOr<void> VulkanContext::create_logical_device_and_queue()
{
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);

    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, queue_families.data());

    bool graphics_queue_family_found = false;
    uint32_t graphics_queue_family_index = 0;
    for (; graphics_queue_family_index < queue_families.size(); ++graphics_queue_family_index) {
        if (queue_families[graphics_queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_found = true;
            break;
        }
    }

    if (!graphics_queue_family_found) {
        return Error::from_string_literal("Graphics queue family not found");
    }

    m_graphics_queue_family = graphics_queue_family_index;

    VkDeviceQueueCreateInfo queue_create_info {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index;
    queue_create_info.queueCount = 1;

    float const queue_priority = 1.0f;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures deviceFeatures {};
#ifdef USE_VULKAN_IMAGES
    Array<const char*, 4> device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
#else
    Array<const char*, 0> device_extensions = {};
#endif
    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;
    create_device_info.enabledExtensionCount = device_extensions.size();
    create_device_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(m_physical_device, &create_device_info, nullptr, &m_logical_device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    vkGetDeviceQueue(m_logical_device, m_graphics_queue_family, 0, &m_graphics_queue);

    return {};
}

#ifdef USE_VULKAN_IMAGES
ErrorOr<void> VulkanContext::create_command_pool()
{
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_graphics_queue_family,
    };

    VkResult result = vkCreateCommandPool(m_logical_device, &command_pool_info, nullptr, &m_command_pool);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateCommandPool returned {}", to_underlying(result));
        return Error::from_string_literal("command pool creation failed");
    }

    return {};
}

ErrorOr<void> VulkanContext::allocate_command_buffer()
{
    VkCommandBufferAllocateInfo command_buffer_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = m_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkResult result = vkAllocateCommandBuffers(m_logical_device, &command_buffer_alloc_info, &m_command_buffer);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateCommandBuffers returned {}", to_underlying(result));
        return Error::from_string_literal("command buffer allocation failed");
    }

    return {};
}

ErrorOr<void> VulkanContext::get_extensions()
{
    auto pfn_vk_get_memory_fd_khr = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(m_logical_device, "vkGetMemoryFdKHR"));
    if (pfn_vk_get_memory_fd_khr == nullptr) {
        return Error::from_string_literal("vkGetMemoryFdKHR unavailable");
    }
    auto pfn_vk_get_image_drm_format_modifier_properties_khr = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vkGetDeviceProcAddr(m_logical_device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (pfn_vk_get_image_drm_format_modifier_properties_khr == nullptr) {
        return Error::from_string_literal("vkGetImageDrmFormatModifierPropertiesEXT unavailable");
    }

    m_ext_procs = {
        .get_memory_fd = pfn_vk_get_memory_fd_khr,
        .get_image_drm_format_modifier_properties = pfn_vk_get_image_drm_format_modifier_properties_khr
    };

    return {};
}

#endif

#if VULKAN_DEBUG

bool VulkanContext::check_layer_support(StringView layer)
{
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    Vector<VkLayerProperties> layers;
    layers.resize(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    for (auto const& layer_properties : layers) {
        if (layer == layer_properties.layerName) {
            return true;
        }
    }

    return false;
}

#endif

#ifdef USE_VULKAN_IMAGES

ErrorOr<NonnullRefPtr<VulkanImage>> VulkanImage::create_shared(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, uint32_t num_modifiers, uint64_t const* modifiers)
{
    VkDrmFormatModifierPropertiesListEXT format_mod_props_list = {};
    format_mod_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    format_mod_props_list.pNext = nullptr;
    VkFormatProperties2 format_props = {};
    format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_props.pNext = &format_mod_props_list;
    vkGetPhysicalDeviceFormatProperties2(context.m_physical_device, format, &format_props);
    Vector<VkDrmFormatModifierPropertiesEXT> format_mod_props;
    format_mod_props.resize(format_mod_props_list.drmFormatModifierCount);
    format_mod_props_list.pDrmFormatModifierProperties = format_mod_props.data();
    vkGetPhysicalDeviceFormatProperties2(context.m_physical_device, format, &format_props);

    // populate a list of all format modifiers that are both renderable and accepted by the caller
    Vector<uint64_t> format_mods;
    for (VkDrmFormatModifierPropertiesEXT const& props : format_mod_props) {
        if ((props.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && (props.drmFormatModifierPlaneCount == 1)) {
            for (uint32_t i = 0; i < num_modifiers; ++i) {
                if (modifiers[i] == props.drmFormatModifier) {
                    format_mods.append(props.drmFormatModifier);
                    break;
                }
            }
        }
    }

    NonnullRefPtr<VulkanImage> image = adopt_ref(*new VulkanImage(context));
    VkImageDrmFormatModifierListCreateInfoEXT image_drm_format_modifier_list_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = static_cast<uint32_t>(format_mods.size()),
        .pDrmFormatModifiers = format_mods.data(),
    };
    VkExternalMemoryImageCreateInfo external_mem_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &image_drm_format_modifier_list_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    Array<uint32_t, 2> queue_families = { context.m_graphics_queue_family, VK_QUEUE_FAMILY_EXTERNAL };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = queue_families.size(),
        .pQueueFamilyIndices = queue_families.data(),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(context.m_logical_device, &image_info, nullptr, &image->m_image);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateImage returned {}", to_underlying(result));
        return Error::from_string_literal("image creation failed");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(context.m_logical_device, image->m_image, &mem_reqs);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(context.m_physical_device, &mem_props);
    uint32_t mem_type_idx;
    for (mem_type_idx = 0; mem_type_idx < mem_props.memoryTypeCount; ++mem_type_idx) {
        if ((mem_reqs.memoryTypeBits & (1 << mem_type_idx)) && (mem_props.memoryTypes[mem_type_idx].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            break;
        }
    }
    if (mem_type_idx == mem_props.memoryTypeCount) {
        return Error::from_string_literal("unable to find suitable image memory type");
    }

    VkExportMemoryAllocateInfo export_mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_mem_alloc_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };
    result = vkAllocateMemory(context.m_logical_device, &mem_alloc_info, nullptr, &image->m_memory);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateMemory returned {}", to_underlying(result));
        return Error::from_string_literal("image memory allocation failed");
    }

    result = vkBindImageMemory(context.m_logical_device, image->m_image, image->m_memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("vkBindImageMemory returned {}", to_underlying(result));
        return Error::from_string_literal("bind image memory failed");
    }

    VkImageSubresource subresource = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout subresource_layout = {};
    vkGetImageSubresourceLayout(context.m_logical_device, image->m_image, &subresource, &subresource_layout);

    VkImageDrmFormatModifierPropertiesEXT image_format_mod_props = {};
    image_format_mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    image_format_mod_props.pNext = nullptr;
    result = context.m_ext_procs.get_image_drm_format_modifier_properties(context.m_logical_device, image->m_image, &image_format_mod_props);
    if (result != VK_SUCCESS) {
        dbgln("vkGetImageDrmFormatModifierPropertiesEXT returned {}", to_underlying(result));
        return Error::from_string_literal("image format modifier retrieval failed");
    }

    // external APIs require general layout
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    image->transition_layout(VK_IMAGE_LAYOUT_UNDEFINED, layout);

    image->m_info = {
        .format = image_info.format,
        .extent = image_info.extent,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .sharing_mode = image_info.sharingMode,
        .layout = layout,
        .row_pitch = subresource_layout.rowPitch,
        .modifier = image_format_mod_props.drmFormatModifier,
    };
    return image;
}

VulkanImage::VulkanImage(VulkanContext const& context)
    : m_context(context)
{
}

VulkanImage::~VulkanImage()
{
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_context.m_logical_device, m_image, nullptr);
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context.m_logical_device, m_memory, nullptr);
    }
}

void VulkanImage::transition_layout(VkImageLayout old_layout, VkImageLayout new_layout)
{
    vkResetCommandBuffer(m_context.m_command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(m_context.m_command_buffer, &begin_info);
    VkImageMemoryBarrier imageMemoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = 0,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(m_context.m_command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &imageMemoryBarrier);
    vkEndCommandBuffer(m_context.m_command_buffer);
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_context.m_command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vkQueueSubmit(m_context.m_graphics_queue, 1, &submit_info, nullptr);
    vkQueueWaitIdle(m_context.m_graphics_queue);
}

int VulkanImage::get_dma_buf_fd()
{
    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = m_memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    VkResult result = m_context.m_ext_procs.get_memory_fd(m_context.m_logical_device, &get_fd_info, &fd);
    if (result != VK_SUCCESS) {
        dbgln("vkGetMemoryFdKHR returned {}", to_underlying(result));
        return -1;
    }
    return fd;
}

#endif

}
