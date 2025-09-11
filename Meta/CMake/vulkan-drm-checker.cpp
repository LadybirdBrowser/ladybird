/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <expected>
#include <iostream>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

using std::cout;
using std::expected;
using std::string;
using std::unexpected;
using std::vector;

static expected<VkInstance, string> create_instance(uint32_t api_version)
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
        cout << "vkCreateInstance returned: " << result << "\n";
        return unexpected(string("Application instance creation failed"));
    }

    return instance;
}

static expected<VkPhysicalDevice, string> pick_physical_device(VkInstance instance)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0)
        return unexpected(string("Can't find any physical devices available"));

    vector<VkPhysicalDevice> devices;
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

    if (picked_device != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(picked_device, &device_properties);

        cout << "Selected Vulkan graphical device: " << device_properties.deviceName << "\n";
        return picked_device;
    }

    return unexpected(string("No physical graphical device selected"));
}

static bool check_device_extension_support(VkPhysicalDevice physicalDevice, vector<string> requiredExtensions)
{
    uint32_t extensionCount = 0;
    // First, get the number of available extensions.
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

    // Then, retrieve the available extensions.
    vector<VkExtensionProperties> availableExtensions;
    availableExtensions.resize(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    // Check if all required extensions are in the list of available extensions.
    bool all_found { true };
    for (auto const& requiredExtension : requiredExtensions) {
        bool found { false };
        for (auto const& availableExtension : availableExtensions) {
            if (requiredExtension == string(availableExtension.extensionName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            cout << "Required device extension not supported: " << requiredExtension << "\n";
            all_found = false;
        }
    }

    return all_found;
}

int main()
{
    uint32_t const api_version = VK_API_VERSION_1_1; // v1.1 needed for vkGetPhysicalDeviceFormatProperties2
    auto instance = create_instance(api_version);
    if (!instance.has_value()) {
        cout << "create_instance failed: " << instance.error() << "\n";
        return (99);
    }

    auto physical_device = pick_physical_device(instance.value());
    if (!physical_device.has_value()) {
        cout << "pick_physical_device failed: " << physical_device.error() << "\n";
        return (98);
    }

    vector<string> device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };

    if (!check_device_extension_support(physical_device.value(), device_extensions))
        return (1);

    return (0);
}
