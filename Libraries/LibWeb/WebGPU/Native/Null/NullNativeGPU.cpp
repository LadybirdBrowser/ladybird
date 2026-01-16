/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGPU/Native/NativeGPU.h>

namespace Web::WebGPU {

struct NativeGPU::Impl { };

WEBGPU_NATIVE_DEFINE_SPECIAL_MEMBERS(NativeGPU);

NativeGPU NativeGPU::create()
{
    return NativeGPU { Impl {} };
}

}
