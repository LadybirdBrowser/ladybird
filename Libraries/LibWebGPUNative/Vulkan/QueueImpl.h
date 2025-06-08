/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebGPUNative/Queue.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);

    ErrorOr<void> submit(Vector<NonnullRawPtr<CommandBuffer>> const&);
    void on_submitted(Function<void()> callback);

private:
    VkQueue m_queue = { VK_NULL_HANDLE };
    Function<void()> m_submitted_callback = { nullptr };
};

}
