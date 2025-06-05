/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/InstanceImpl.h>
#include <VkBootstrap.h>

namespace WebGPUNative {

Instance::Impl::~Impl()
{
    if (m_vkb_instance.instance) {
        vkb::destroy_instance(m_vkb_instance);
    }
}

ErrorOr<void> Instance::Impl::initialize()
{
    vkb::InstanceBuilder builder;

    builder.set_app_name("Ladybird WebGPU Native")
        .set_app_version(0, 1, 0)
        .set_engine_name("Ladybird WebGPU Native")
        .set_engine_version(0, 1, 0)
        .require_api_version(1, 0, 0);

#ifdef WEBGPUNATIVE_DEBUG
    builder.request_validation_layers(true)
        .use_default_debug_messenger();
#endif

    auto instance_result = builder.build();
    if (!instance_result.has_value()) {
        return make_error(instance_result.vk_result(), "Unable to create instance");
    }

    m_vkb_instance = instance_result.value();
    return {};
}

VkInstance Instance::Impl::instance() const
{
    return m_vkb_instance.instance;
}

}
