/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/CommandBuffer.h>
#include <LibWebGPUNative/Metal/Handle.h>

namespace WebGPUNative {

struct CommandBuffer::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder);
    ~Impl() = default;

    id command_buffer() const { return m_command_buffer->get(); }

private:
    OwnPtr<MetalCommandBufferHandle> m_command_buffer;
};

}
