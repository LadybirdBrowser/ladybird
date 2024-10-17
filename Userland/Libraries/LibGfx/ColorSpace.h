/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <LibIPC/Forward.h>

#include <core/SkColorSpace.h>

namespace Gfx {

class ColorSpace {
public:
    ColorSpace() = default;
    AK_MAKE_DEFAULT_COPYABLE(ColorSpace);
    AK_MAKE_DEFAULT_MOVABLE(ColorSpace);
    ~ColorSpace() = default;

    static ErrorOr<ColorSpace> load_from_icc_bytes(ReadonlyBytes);

    auto& color_space()
    {
        return m_color_space;
    }

private:
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, ColorSpace const&);
    friend ErrorOr<ColorSpace> IPC::decode(IPC::Decoder&);

    explicit ColorSpace(sk_sp<SkColorSpace>&& ColorSpace);

    sk_sp<SkColorSpace> m_color_space { nullptr };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ColorSpace const&);

template<>
ErrorOr<Gfx::ColorSpace> decode(Decoder&);

}
