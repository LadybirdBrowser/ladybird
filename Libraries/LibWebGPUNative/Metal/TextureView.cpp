/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/TextureViewImpl.h>
#include <LibWebGPUNative/TextureView.h>

namespace WebGPUNative {

TextureView::TextureView(Texture const& gpu_texture)
    : m_impl(make<Impl>(gpu_texture))
{
}

TextureView::TextureView(TextureView&&) noexcept = default;
TextureView& TextureView::operator=(TextureView&&) noexcept = default;
TextureView::~TextureView() = default;

ErrorOr<void> TextureView::initialize()
{
    return m_impl->initialize();
}

}
