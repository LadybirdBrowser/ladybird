/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Metal/Handle.h>

namespace WebGPUNative {

struct CommandEncoder::Impl {
    explicit Impl(Device const& gpu_device);
    ~Impl() = default;

    ErrorOr<void> initialize();

    id command_buffer() const { return m_command_buffer->get(); }

private:
    OwnPtr<MetalCommandQueueHandle> m_command_queue;
    OwnPtr<MetalCommandBufferHandle> m_command_buffer;
};

}
