/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Gfx {

namespace Details {

struct ColorSpaceImpl;

}

class ColorSpace {
public:
    ColorSpace();
    ColorSpace(ColorSpace const&);
    ColorSpace(ColorSpace&&);
    ColorSpace& operator=(ColorSpace const&);
    ColorSpace& operator=(ColorSpace&&);
    ~ColorSpace();

    static ErrorOr<ColorSpace> from_cicp(Media::CodingIndependentCodePoints);
    static ErrorOr<ColorSpace> load_from_icc_bytes(ReadonlyBytes);

    // In order to keep this file free of Skia types, this function can't return
    // a sk_sp<ColorSpace>. To work around that issue, we define a template here
    // and only provide a specialization for sk_sp<SkColorSpace>.
    template<typename T>
    T& color_space();

private:
    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    explicit ColorSpace(NonnullOwnPtr<Details::ColorSpaceImpl>&& color_pace);

    NonnullOwnPtr<Details::ColorSpaceImpl> m_color_space;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ColorSpace const&);

template<>
ErrorOr<Gfx::ColorSpace> decode(Decoder&);

}
