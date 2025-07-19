/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebGPUNative/Vulkan/AdapterImpl.h>
#include <LibWebGPUNative/Vulkan/DeviceImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>

namespace WebGPUNative {

Device::Impl::Impl(Adapter const& gpu_adapter)
    : m_physical_device(gpu_adapter.m_impl->physical_device())
{
}

Device::Impl::~Impl()
{
    vkDestroyCommandPool(m_logical_device, m_command_pool, nullptr);
    vkDestroyDevice(m_logical_device, nullptr);
}

ErrorOr<void> Device::Impl::initialize()
{
    // TODO: Hardcode the graphics queue for now and create the logical device
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);

    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, queue_families.data());

    Optional<uint32_t> queue_family_index;
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        // FIXME: Support all queue types
        auto const& queue_family = queue_families[i];
        if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_index = i;
            break;
        }
    }
    if (!queue_family_index.has_value())
        return make_error("No supported queue family available");

    constexpr float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = queue_family_index.value();
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;

    constexpr VkPhysicalDeviceFeatures physical_device_features = {};

    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.pEnabledFeatures = &physical_device_features;

    if (VkResult const create_device_result = vkCreateDevice(m_physical_device, &device_create_info, nullptr, &m_logical_device); create_device_result != VK_SUCCESS) {
        return make_error(create_device_result, "Unable to create device");
    }
    vkGetDeviceQueue(m_logical_device, queue_family_index.value(), 0, &m_queue);

    VkCommandPoolCreateInfo command_pool_create_info = {};
    command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_create_info.queueFamilyIndex = queue_family_index.value();
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (VkResult const create_command_pool_result = vkCreateCommandPool(m_logical_device, &command_pool_create_info, nullptr, &m_command_pool); create_command_pool_result != VK_SUCCESS)
        return make_error(create_command_pool_result, "Unable to create command pool");
    return {};
}

}
