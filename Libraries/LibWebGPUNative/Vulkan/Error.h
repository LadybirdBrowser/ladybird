/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

template<size_t N>
Error make_error(VkResult const result, char const (&message)[N])
{
    auto const error_string = MUST(String::formatted("WebGPUNative [{}]: {}", string_VkResult(result), message));
    return Error::from_string_view(error_string.bytes_as_string_view());
}

template<size_t N>
Error make_error(char const (&message)[N])
{
    auto const error_string = MUST(String::formatted("WebGPUNative [Vulkan]: {}", message));
    return Error::from_string_view(error_string.bytes_as_string_view());
}

}
