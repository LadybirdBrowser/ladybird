/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Texture;

class WEBGPUNATIVE_API TextureView {
public:
    friend class CommandEncoder;

    explicit TextureView(Texture const&);
    TextureView(TextureView&&) noexcept;
    TextureView& operator=(TextureView&&) noexcept;
    ~TextureView();

    ErrorOr<void> initialize();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
