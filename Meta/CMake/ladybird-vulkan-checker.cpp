/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <vulkan/vulkan.h>

int main(int /* argc */, char** /* argv */)
{
    uint32_t const api_version = VK_API_VERSION_1_1; // v1.1 needed for vkGetPhysicalDeviceFormatProperties2
    auto* instance = TRY(create_instance(api_version));
    auto* physical_device = TRY(pick_physical_device(instance));

    uint32_t graphics_queue_family = 0;
    auto* logical_device = TRY(create_logical_device(physical_device, &graphics_queue_family));

    if (logical_device.is_error())
        exit(1);

    exit(0);
}

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

static bool check_device_extension_support(VkPhysicalDevice physicalDevice, Vector<char const*>& requiredExtensions)
{
    uint32_t extensionCount = 0;
    // First, get the number of available extensions.
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

    // Then, retrieve the available extensions.
    Vector<VkExtensionProperties> availableExtensions;
    availableExtensions.resize(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    // Check if all required extensions are in the list of available extensions.
    bool all_found { true };
    for (auto const& requiredExtension : requiredExtensions) {
        bool found { false };
        for (auto const& availableExtension : availableExtensions) {
            if (strcmp(requiredExtension, availableExtension.extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            dbgln("Required device extension not supported: {} ", requiredExtension);
            all_found = false;
        }
    }

    return all_found;
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

    Vector<char const*> device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    uint32_t device_extension_count = device_extensions.size();

    if (!check_device_extension_support(physical_device, device_extensions)) {
        return Error::from_string_literal("Physical device lacking extension(s)");
    }

    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;
    create_device_info.enabledExtensionCount = device_extension_count;
    create_device_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device, &create_device_info, nullptr, &device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    return device;
}
