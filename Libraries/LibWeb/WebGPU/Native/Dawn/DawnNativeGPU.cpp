/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWeb/WebGPU/Native/NativeGPU.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

struct NativeGPU::Impl {
    wgpu::Instance m_instance { nullptr };
};

WEBGPU_NATIVE_DEFINE_SPECIAL_MEMBERS(NativeGPU);

NativeGPU NativeGPU::create()
{
    Vector const required_features = {
        // See Callback Reentrancy section at https://webgpu-native.github.io/webgpu-headers/Asynchronous-Operations.html#Process-Events
        // See WGPUCallbackMode enum descriptions at https://webgpu-native.github.io/webgpu-headers/group__Enumerations.html
        // This ensures we get the implicit safety and avoid undefined behaviour, so we will just use WaitAnyOnly inside an EventLoopPlugin::deferred_invoke()
        // for executing asynchronous operations on the instance with an infinite timeout.
        wgpu::InstanceFeatureName::TimedWaitAny,
    };

    wgpu::InstanceDescriptor instance_descriptor {};
    instance_descriptor.requiredFeatureCount = required_features.size();
    instance_descriptor.requiredFeatures = required_features.data();

    return NativeGPU { Impl { .m_instance = wgpu::CreateInstance(&instance_descriptor) } };
}

}
