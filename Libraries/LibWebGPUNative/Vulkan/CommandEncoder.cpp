/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/CommandBuffer.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>

namespace WebGPUNative {

CommandEncoder::CommandEncoder(Device const& gpu_device)
    : m_impl(make<Impl>(gpu_device))
{
}

CommandEncoder::CommandEncoder(CommandEncoder&&) noexcept = default;
CommandEncoder& CommandEncoder::operator=(CommandEncoder&&) noexcept = default;
CommandEncoder::~CommandEncoder() = default;

ErrorOr<void> CommandEncoder::initialize()
{
    return m_impl->initialize();
}

}
