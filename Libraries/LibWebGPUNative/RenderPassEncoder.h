/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Vector.h>
#include <LibWebGPUNative/TextureView.h>

namespace WebGPUNative {

class CommandEncoder;

struct Color {
    double r, g, b, a = { 0.0 };
};

struct RenderPassColorAttachment {
    NonnullRawPtr<TextureView> view;
    Optional<Color> clear_value;
};

struct RenderPassDescriptor {
    Vector<RenderPassColorAttachment> color_attachments;
};

class WEBGPUNATIVE_API RenderPassEncoder {
public:
    friend class CommandEncoder;

    explicit RenderPassEncoder(CommandEncoder const&, RenderPassDescriptor const&);
    RenderPassEncoder(RenderPassEncoder&&);
    RenderPassEncoder& operator=(RenderPassEncoder&&);
    ~RenderPassEncoder();

    ErrorOr<void> initialize();

    RenderPassDescriptor const& render_pass_descriptor() const;

    void end();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
