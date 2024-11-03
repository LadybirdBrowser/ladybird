/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

class AvailableSize {
public:
    enum class Type {
        Definite,
        Indefinite,
        MinContent,
        MaxContent,
    };

    static AvailableSize make_definite(CSSPixels);
    static AvailableSize make_indefinite();
    static AvailableSize make_min_content();
    static AvailableSize make_max_content();

    bool is_definite() const { return m_type == Type::Definite; }
    bool is_indefinite() const { return m_type == Type::Indefinite; }
    bool is_min_content() const { return m_type == Type::MinContent; }
    bool is_max_content() const { return m_type == Type::MaxContent; }
    bool is_intrinsic_sizing_constraint() const { return is_min_content() || is_max_content(); }

    CSSPixels to_px_or_zero() const
    {
        if (!is_definite())
            return 0;
        return m_value;
    }

    String to_string() const;

    bool operator==(AvailableSize const& other) const = default;
    bool operator<(AvailableSize const& other) const { return m_value < other.m_value; }

private:
    AvailableSize(Type type, CSSPixels);

    Type m_type {};
    CSSPixels m_value {};
};

inline bool operator>(CSSPixels left, AvailableSize const& right)
{
    if (right.is_max_content() || right.is_indefinite())
        return false;
    if (right.is_min_content())
        return true;
    return left > right.to_px_or_zero();
}

inline bool operator<(CSSPixels left, AvailableSize const& right)
{
    if (right.is_max_content() || right.is_indefinite())
        return true;
    if (right.is_min_content())
        return false;
    return left < right.to_px_or_zero();
}

inline bool operator<(AvailableSize const& left, CSSPixels right)
{
    if (left.is_min_content())
        return true;
    if (left.is_max_content() || left.is_indefinite())
        return false;
    return left.to_px_or_zero() < right;
}

class AvailableSpace {
public:
    AvailableSpace(AvailableSize w, AvailableSize h)
        : width(move(w))
        , height(move(h))
    {
    }

    bool operator==(AvailableSpace const& other) const = default;

    AvailableSize width;
    AvailableSize height;

    String to_string() const;
};

}

template<>
struct AK::Formatter<Web::Layout::AvailableSize> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Layout::AvailableSize const& available_size)
    {
        return Formatter<StringView>::format(builder, available_size.to_string());
    }
};

template<>
struct AK::Formatter<Web::Layout::AvailableSpace> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Layout::AvailableSpace const& available_space)
    {
        return Formatter<StringView>::format(builder, available_space.to_string());
    }
};
