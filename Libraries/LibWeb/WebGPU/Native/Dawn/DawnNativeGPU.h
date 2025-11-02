/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGPU/Native/NativeGPU.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

struct NativeGPU::Impl {
    static ErrorOr<wgpu::Instance> create();

    wgpu::Instance m_instance { nullptr };
};

}
