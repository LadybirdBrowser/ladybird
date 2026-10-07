/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Typeface.h>

namespace Gfx {

class TypefaceSkia : public Gfx::Typeface {
    AK_MAKE_NONCOPYABLE(TypefaceSkia);

public:
    static ErrorOr<NonnullRefPtr<TypefaceSkia>> load_from_buffer(ReadonlyBytes, u32 ttc_index, NonnullRefPtr<FontDataBacking>);

    virtual ReadonlyBytes buffer() const LIFETIME_BOUND override { return m_buffer; }
    virtual u32 ttc_index() const override { return m_ttc_index; }

    SkTypeface const* sk_typeface() const;

private:
    struct Impl;
    Impl& impl() const { return *m_impl; }
    NonnullOwnPtr<Impl> m_impl;

    TypefaceSkia(NonnullOwnPtr<Impl>, ReadonlyBytes, u32 ttc_index = 0);

    virtual bool is_skia() const override { return true; }

    ReadonlyBytes m_buffer;
    u32 m_ttc_index { 0 };
};

template<>
inline bool Typeface::fast_is<TypefaceSkia>() const { return is_skia(); }

}
