/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWebGPUNative/Vulkan/AdapterImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/InstanceImpl.h>

namespace WebGPUNative {

Adapter::Impl::Impl(Instance const& gpu)
    : m_instance(gpu.m_impl->instance())
{
}

Adapter::Impl::~Impl() = default;

ErrorOr<void> Adapter::Impl::initialize()
{
    uint32_t physical_device_count = 0;
    vkEnumeratePhysicalDevices(m_instance, &physical_device_count, nullptr);
    if (physical_device_count == 0)
        return make_error("No physical devices found");

    // FIXME: Expose and acknowledge options for guiding adapter selection
    //  https://www.w3.org/TR/webgpu/#adapter-selection

    Vector<VkPhysicalDevice> physical_devices;
    physical_devices.resize(physical_device_count);
    vkEnumeratePhysicalDevices(m_instance, &physical_device_count, physical_devices.data());

#ifdef WEBGPU_DEBUG
    dbgln("Number of physical devices: {}", physical_device_count);
#endif

    VkPhysicalDevice selected_device = VK_NULL_HANDLE;
    // FIXME: Low powerPreference should map to an integrated GPU, otherwise use discrete GPU
    for (auto const& device : physical_devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        // FIXME: Support all physical device types
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected_device = device;
            break;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            selected_device = device;
        }
    }

    if (selected_device == VK_NULL_HANDLE)
        return make_error("No supported physical devices available");

    VkPhysicalDeviceProperties selected_properties;
    vkGetPhysicalDeviceProperties(selected_device, &selected_properties);
#ifdef WEBGPU_DEBUG
    dbgln("Selected physical device: {}", selected_properties.deviceName);
#endif
    m_physical_device = selected_device;

    // FIXME: Mark selected device as consumed

    return {};
}

}
