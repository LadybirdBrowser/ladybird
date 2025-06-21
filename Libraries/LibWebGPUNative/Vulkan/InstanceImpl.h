/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Instance.h>
#include <VkBootstrap.h>

namespace WebGPUNative {

struct Instance::Impl {
    ~Impl();

    ErrorOr<void> initialize();

    VkInstance instance() const;

private:
    vkb::Instance m_vkb_instance {};
};

}
