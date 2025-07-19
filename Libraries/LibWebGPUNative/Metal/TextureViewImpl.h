/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGfx/Size.h>
#include <LibWebGPUNative/Metal/Handle.h>
#include <LibWebGPUNative/TextureView.h>

namespace WebGPUNative {

struct TextureView::Impl {
    explicit Impl(Texture const& gpu_texture);
    ~Impl() = default;

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }
    id texture_view() const { return m_texture_view->get(); }

private:
    Gfx::IntSize m_size;
    OwnPtr<MetalTextureHandle> m_texture;
    OwnPtr<MetalTextureHandle> m_texture_view;
};

}
