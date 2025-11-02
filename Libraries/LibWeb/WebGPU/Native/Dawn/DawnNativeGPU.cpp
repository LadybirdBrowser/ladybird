/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWeb/WebGPU/Native/Dawn/DawnNativeGPU.h>

namespace Web::WebGPU {

WEBGPU_NATIVE_DEFINE_SPECIAL_MEMBERS(NativeGPU);

ErrorOr<wgpu::Instance> NativeGPU::Impl::create()
{
    Vector const required_features = {
        // See Callback Reentrancy section at https://webgpu-native.github.io/webgpu-headers/Asynchronous-Operations.html#Process-Events
        // See WGPUCallbackMode enum descriptions at https://webgpu-native.github.io/webgpu-headers/group__Enumerations.html#ggaf6f2496c9c727391ba83e928a8d4e63ea4395a0937cc3c5ab20eb6f2ae659c509
        // This ensures we get the implicit safety and avoid undefined behaviour, initially we will just use WaitAnyOnly for executing asynchronous operations on the instance with an infinite timeout.
        wgpu::InstanceFeatureName::TimedWaitAny,
    };

    wgpu::InstanceDescriptor instance_descriptor {};
    instance_descriptor.requiredFeatureCount = required_features.size();
    instance_descriptor.requiredFeatures = required_features.data();
    wgpu::Instance instance = wgpu::CreateInstance(&instance_descriptor);
    if (instance == nullptr)
        return Error::from_string_literal("Unable to create GPU instance");
    return instance;
}

ErrorOr<NativeGPU> NativeGPU::create()
{
    auto instance = TRY(Impl::create());
    return NativeGPU { Impl { .m_instance = instance } };
}

}
