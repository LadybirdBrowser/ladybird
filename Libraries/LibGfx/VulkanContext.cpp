/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibGfx/VulkanContext.h>

namespace Gfx {

static ErrorOr<VkInstance> create_instance(uint32_t api_version)
{
    VkInstance instance;

    VkApplicationInfo app_info {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Ladybird";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = nullptr;
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = api_version;

    VkInstanceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    auto result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateInstance returned {}", to_underlying(result));
        return Error::from_string_literal("Application instance creation failed");
    }

    return instance;
}

static ErrorOr<VkPhysicalDevice> pick_physical_device(VkInstance instance)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0)
        return Error::from_string_literal("Can't find any physical devices available");

    Vector<VkPhysicalDevice> devices;
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    VkPhysicalDevice picked_device = VK_NULL_HANDLE;
    // Pick discrete GPU or the first device in the list
    for (auto const& device : devices) {
        if (picked_device == VK_NULL_HANDLE)
            picked_device = device;

        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);
        if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            picked_device = device;
    }

    if (picked_device != VK_NULL_HANDLE)
        return picked_device;

    VERIFY_NOT_REACHED();
}

static ErrorOr<VkDevice> create_logical_device(VkPhysicalDevice physical_device, uint32_t* graphics_queue_family)
{
    VkDevice device;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

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

    *graphics_queue_family = graphics_queue_family_index;

    VkDeviceQueueCreateInfo queue_create_info {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index;
    queue_create_info.queueCount = 1;

    float const queue_priority = 1.0f;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures deviceFeatures {};

    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;

    if (vkCreateDevice(physical_device, &create_device_info, nullptr, &device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    return device;
}

static ErrorOr<VkCommandPool> create_command_pool(VkDevice logical_device, uint32_t queue_family_index)
{
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(logical_device, &command_pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateCommandPool returned {}", to_underlying(result));
        return Error::from_string_literal("command pool creation failed");
    }
    return command_pool;
}

static ErrorOr<VkCommandBuffer> allocate_command_buffer(VkDevice logical_device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo command_buffer_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(logical_device, &command_buffer_alloc_info, &command_buffer);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateCommandBuffers returned {}", to_underlying(result));
        return Error::from_string_literal("command buffer allocation failed");
    }
    return command_buffer;
}

ErrorOr<VulkanContext> create_vulkan_context()
{
    uint32_t const api_version = VK_API_VERSION_1_0;
    auto* instance = TRY(create_instance(api_version));
    auto* physical_device = TRY(pick_physical_device(instance));

    uint32_t graphics_queue_family = 0;
    auto* logical_device = TRY(create_logical_device(physical_device, &graphics_queue_family));
    VkQueue graphics_queue;
    vkGetDeviceQueue(logical_device, graphics_queue_family, 0, &graphics_queue);

    VkCommandPool command_pool = TRY(create_command_pool(logical_device, graphics_queue_family));
    VkCommandBuffer command_buffer = TRY(allocate_command_buffer(logical_device, command_pool));

    return VulkanContext {
        .api_version = api_version,
        .instance = instance,
        .physical_device = physical_device,
        .logical_device = logical_device,
        .graphics_queue = graphics_queue,
        .graphics_queue_family = graphics_queue_family,
        .command_pool = command_pool,
        .command_buffer = command_buffer,
    };
}

}
