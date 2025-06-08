/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class CommandBuffer;
class Device;
struct RenderPassDescriptor;
class RenderPassEncoder;

class WEBGPUNATIVE_API CommandEncoder {
public:
    friend class CommandBuffer;
    friend class Device;
    friend class RenderPassEncoder;

    explicit CommandEncoder(Device const&);
    CommandEncoder(CommandEncoder&&) noexcept;
    CommandEncoder& operator=(CommandEncoder&&) noexcept;
    ~CommandEncoder();

    ErrorOr<void> initialize();

    ErrorOr<RenderPassEncoder> begin_render_pass(RenderPassDescriptor const&) const;

    ErrorOr<CommandBuffer> finish();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
