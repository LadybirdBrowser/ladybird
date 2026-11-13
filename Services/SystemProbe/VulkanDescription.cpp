/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPlatformFeatures/Build.h>
#include <SystemProbe/VulkanDescription.h>
#include <SystemProbe/VulkanVideoDescription.h>
#include <string.h>

static String string_from_driver(char const* characters, size_t capacity)
{
    return String::from_utf8_with_replacement_character({ characters, strnlen(characters, capacity) });
}

Optional<PlatformFeatures::GpuFacts> describe_gpu_through_vulkan(Optional<Gfx::GpuDeviceIdentity> const& preferred_device)
{
    VkApplicationInfo application_info {};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Ladybird";
    application_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    // The instance is never destroyed, since tearing the driver down only delays the exit.
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        return {};

    auto physical_device = Gfx::pick_physical_device(instance, preferred_device);
    if (physical_device.is_error())
        return {};

    VkPhysicalDeviceProperties basic_properties;
    vkGetPhysicalDeviceProperties(physical_device.value(), &basic_properties);

    VkPhysicalDeviceDriverProperties driver_properties {};
    driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    // Drivers describe themselves from Vulkan 1.2.
    if (basic_properties.apiVersion >= VK_API_VERSION_1_2)
        properties.pNext = &driver_properties;
    vkGetPhysicalDeviceProperties2(physical_device.value(), &properties);

    PlatformFeatures::GpuFacts gpu;
    gpu.vendor_id = properties.properties.vendorID;
    gpu.device_id = properties.properties.deviceID;
    gpu.device_name = string_from_driver(properties.properties.deviceName, sizeof(properties.properties.deviceName));
    gpu.driver_id = driver_properties.driverID;
    gpu.driver_name = string_from_driver(driver_properties.driverName, sizeof(driver_properties.driverName));
    gpu.driver_info = string_from_driver(driver_properties.driverInfo, sizeof(driver_properties.driverInfo));
    gpu.driver_version = properties.properties.driverVersion;
    gpu.api_version = properties.properties.apiVersion;
    gpu.device_identity = MUST(String::from_byte_string(Gfx::gpu_device_identity_of(physical_device.value()).to_byte_string()));
    gpu.supports_ycbcr_sampling = Gfx::device_can_sample_through_ycbcr_conversion(physical_device.value());
    if constexpr (PlatformFeatures::this_build.decodes_video_through_vulkan())
        describe_video_decoding_through_vulkan(gpu, instance, physical_device.value());
    else
        gpu.cannot_decode_video_because = "Video is not decoded through Vulkan on this platform"_string;
    return gpu;
}
