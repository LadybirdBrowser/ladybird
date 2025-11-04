/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Orientation.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <math.h>

namespace Gfx {

template<typename T>
class Rect {
public:
    Rect() = default;

    Rect(T x, T y, T width, T height)
        : m_location(x, y)
        , m_size(width, height)
    {
    }

    template<typename U>
    Rect(U x, U y, U width, U height)
        : m_location(x, y)
        , m_size(width, height)
    {
    }

    Rect(Point<T> const& location, Size<T> const& size)
        : m_location(location)
        , m_size(size)
    {
    }

    template<typename U>
    Rect(Point<U> const& location, Size<U> const& size)
        : m_location(location)
        , m_size(size)
    {
    }

    template<typename U>
    explicit Rect(Rect<U> const& other)
        : m_location(other.location())
        , m_size(other.size())
    {
    }

    [[nodiscard]] ALWAYS_INLINE T x() const { return location().x(); }
    [[nodiscard]] ALWAYS_INLINE T y() const { return location().y(); }
    [[nodiscard]] ALWAYS_INLINE T width() const { return m_size.width(); }
    [[nodiscard]] ALWAYS_INLINE T height() const { return m_size.height(); }

    ALWAYS_INLINE void set_x(T x) { m_location.set_x(x); }
    ALWAYS_INLINE void set_y(T y) { m_location.set_y(y); }
    ALWAYS_INLINE void set_width(T width) { m_size.set_width(width); }
    ALWAYS_INLINE void set_height(T height) { m_size.set_height(height); }

    [[nodiscard]] ALWAYS_INLINE Point<T> const& location() const { return m_location; }
    [[nodiscard]] ALWAYS_INLINE Size<T> const& size() const { return m_size; }

    [[nodiscard]] ALWAYS_INLINE bool is_empty() const { return width() <= 0 || height() <= 0; }

    ALWAYS_INLINE void translate_by(T dx, T dy) { m_location.translate_by(dx, dy); }
    ALWAYS_INLINE void translate_by(T dboth) { m_location.translate_by(dboth); }
    ALWAYS_INLINE void translate_by(Point<T> const& delta) { m_location.translate_by(delta); }

    ALWAYS_INLINE void scale_by(T dx, T dy)
    {
        m_location.scale_by(dx, dy);
        m_size.scale_by(dx, dy);
    }
    ALWAYS_INLINE void scale_by(T dboth) { scale_by(dboth, dboth); }
    ALWAYS_INLINE void scale_by(Point<T> const& delta) { scale_by(delta.x(), delta.y()); }

    void transform_by(AffineTransform const& transform) { *this = transform.map(*this); }

    [[nodiscard]] Point<T> center() const
    {
        return { x() + width() / 2, y() + height() / 2 };
    }

    ALWAYS_INLINE void set_location(Point<T> const& location)
    {
        m_location = location;
    }

    ALWAYS_INLINE void set_size(Size<T> const& size)
    {
        m_size = size;
    }

    void set_size(T width, T height)
    {
        m_size.set_width(width);
        m_size.set_height(height);
    }

    void inflate(T w, T h)
    {
        set_x(x() - w / 2);
        set_width(width() + w);
        set_y(y() - h / 2);
        set_height(height() + h);
    }

    void inflate(T top, T right, T bottom, T left)
    {
        set_x(x() - left);
        set_width(width() + left + right);
        set_y(y() - top);
        set_height(height() + top + bottom);
    }

    void inflate(Size<T> const& size)
    {
        set_x(x() - size.width() / 2);
        set_width(width() + size.width());
        set_y(y() - size.height() / 2);
        set_height(height() + size.height());
    }

    void shrink(T w, T h)
    {
        set_x(x() + w / 2);
        set_width(width() - w);
        set_y(y() + h / 2);
        set_height(height() - h);
    }

    void shrink(T top, T right, T bottom, T left)
    {
        set_x(x() + left);
        set_width(width() - (left + right));
        set_y(y() + top);
        set_height(height() - (top + bottom));
    }

    void shrink(Size<T> const& size)
    {
        set_x(x() + size.width() / 2);
        set_width(width() - size.width());
        set_y(y() + size.height() / 2);
        set_height(height() - size.height());
    }

    [[nodiscard]] Rect<T> translated(T dx, T dy) const
    {
        Rect<T> rect = *this;
        rect.translate_by(dx, dy);
        return rect;
    }

    [[nodiscard]] Rect<T> translated(T dboth) const
    {
        Rect<T> rect = *this;
        rect.translate_by(dboth);
        return rect;
    }

    [[nodiscard]] Rect<T> translated(Point<T> const& delta) const
    {
        Rect<T> rect = *this;
        rect.translate_by(delta);
        return rect;
    }

    [[nodiscard]] Rect<T> scaled(T dboth) const
    {
        Rect<T> rect = *this;
        rect.scale_by(dboth);
        return rect;
    }

    [[nodiscard]] Rect<T> scaled(T sx, T sy) const
    {
        Rect<T> rect = *this;
        rect.scale_by(sx, sy);
        return rect;
    }

    [[nodiscard]] Rect<T> scaled(Point<T> const& s) const
    {
        Rect<T> rect = *this;
        rect.scale_by(s);
        return rect;
    }

    [[nodiscard]] Rect<T> transformed(AffineTransform const& transform) const
    {
        Rect<T> rect = *this;
        rect.transform_by(transform);
        return rect;
    }

    [[nodiscard]] Rect<T> shrunken(T w, T h) const
    {
        Rect<T> rect = *this;
        rect.shrink(w, h);
        return rect;
    }

    [[nodiscard]] Rect<T> shrunken(T top, T right, T bottom, T left) const
    {
        Rect<T> rect = *this;
        rect.shrink(top, right, bottom, left);
        return rect;
    }

    [[nodiscard]] Rect<T> shrunken(Size<T> const& size) const
    {
        Rect<T> rect = *this;
        rect.shrink(size);
        return rect;
    }

    [[nodiscard]] Rect<T> inflated(T w, T h) const
    {
        Rect<T> rect = *this;
        rect.inflate(w, h);
        return rect;
    }

    [[nodiscard]] Rect<T> inflated(T top, T right, T bottom, T left) const
    {
        Rect<T> rect = *this;
        rect.inflate(top, right, bottom, left);
        return rect;
    }

    [[nodiscard]] Rect<T> inflated(Size<T> const& size) const
    {
        Rect<T> rect = *this;
        rect.inflate(size);
        return rect;
    }

    Rect<T> take_from_right(T w)
    {
        if (w > width())
            w = width();
        Rect<T> rect = *this;
        set_width(width() - w);
        rect.set_x(x() + width());
        rect.set_width(w);
        return rect;
    }

    Rect<T> take_from_left(T w)
    {
        if (w > width())
            w = width();
        Rect<T> rect = *this;
        set_x(x() + w);
        set_width(width() - w);
        rect.set_width(w);
        return rect;
    }

    Rect<T> take_from_top(T h)
    {
        if (h > height())
            h = height();
        Rect<T> rect = *this;
        set_y(y() + h);
        set_height(height() - h);
        rect.set_height(h);
        return rect;
    }

    Rect<T> take_from_bottom(T h)
    {
        if (h > height())
            h = height();
        Rect<T> rect = *this;
        set_height(height() - h);
        rect.set_y(y() + height());
        rect.set_height(h);
        return rect;
    }

    [[nodiscard]] bool contains_vertically(T y) const
    {
        return y >= top() && y < bottom();
    }

    [[nodiscard]] bool contains_horizontally(T x) const
    {
        return x >= left() && x < right();
    }

    [[nodiscard]] bool contains(T x, T y) const
    {
        return contains_horizontally(x) && contains_vertically(y);
    }

    [[nodiscard]] ALWAYS_INLINE bool contains(Point<T> const& point) const
    {
        return contains(point.x(), point.y());
    }

    [[nodiscard]] bool contains(Rect<T> const& other) const
    {
        return left() <= other.left()
            && right() >= other.right()
            && top() <= other.top()
            && bottom() >= other.bottom();
    }

    template<typename Container>
    [[nodiscard]] bool contains(Container const& others) const
    {
        bool have_any = false;
        for (auto const& other : others) {
            if (!contains(other))
                return false;
            have_any = true;
        }
        return have_any;
    }

    [[nodiscard]] ALWAYS_INLINE T primary_offset_for_orientation(Orientation orientation) const { return m_location.primary_offset_for_orientation(orientation); }
    ALWAYS_INLINE void set_primary_offset_for_orientation(Orientation orientation, T value) { m_location.set_primary_offset_for_orientation(orientation, value); }
    [[nodiscard]] ALWAYS_INLINE T secondary_offset_for_orientation(Orientation orientation) const { return m_location.secondary_offset_for_orientation(orientation); }
    ALWAYS_INLINE void set_secondary_offset_for_orientation(Orientation orientation, T value) { m_location.set_secondary_offset_for_orientation(orientation, value); }

    [[nodiscard]] ALWAYS_INLINE T primary_size_for_orientation(Orientation orientation) const { return m_size.primary_size_for_orientation(orientation); }
    [[nodiscard]] ALWAYS_INLINE T secondary_size_for_orientation(Orientation orientation) const { return m_size.secondary_size_for_orientation(orientation); }
    ALWAYS_INLINE void set_primary_size_for_orientation(Orientation orientation, T value) { m_size.set_primary_size_for_orientation(orientation, value); }
    ALWAYS_INLINE void set_secondary_size_for_orientation(Orientation orientation, T value) { m_size.set_secondary_size_for_orientation(orientation, value); }

    [[nodiscard]] T first_edge_for_orientation(Orientation orientation) const
    {
        if (orientation == Orientation::Vertical)
            return top();
        return left();
    }

    [[nodiscard]] T last_edge_for_orientation(Orientation orientation) const
    {
        if (orientation == Orientation::Vertical)
            return bottom();
        return right();
    }

    [[nodiscard]] ALWAYS_INLINE T left() const { return x(); }
    [[nodiscard]] ALWAYS_INLINE T right() const { return x() + width(); }
    [[nodiscard]] ALWAYS_INLINE T top() const { return y(); }
    [[nodiscard]] ALWAYS_INLINE T bottom() const { return y() + height(); }

    ALWAYS_INLINE void set_left(T left) { set_x(left); }
    ALWAYS_INLINE void set_top(T top) { set_y(top); }
    ALWAYS_INLINE void set_right(T right) { set_width(right - x()); }
    ALWAYS_INLINE void set_bottom(T bottom) { set_height(bottom - y()); }

    void set_right_without_resize(T new_right)
    {
        auto delta = new_right - right();
        translate_by(delta, 0);
    }

    void set_bottom_without_resize(T new_bottom)
    {
        auto delta = new_bottom - bottom();
        translate_by(0, delta);
    }

    [[nodiscard]] bool intersects_vertically(Rect<T> const& other) const
    {
        return top() < other.bottom() && other.top() < bottom();
    }

    [[nodiscard]] bool intersects_horizontally(Rect<T> const& other) const
    {
        return left() < other.right() && other.left() < right();
    }

    [[nodiscard]] bool intersects(Rect<T> const& other) const
    {
        return left() < other.right()
            && other.left() < right()
            && top() < other.bottom()
            && other.top() < bottom();
    }

    [[nodiscard]] bool edge_adjacent_intersects(Rect<T> const& other) const
    {
        return max(left(), other.left()) <= min(right(), other.right())
            && max(top(), other.top()) <= min(bottom(), other.bottom());
    }

    template<typename Container>
    [[nodiscard]] bool intersects(Container const& others) const
    {
        for (auto const& other : others) {
            if (intersects(other))
                return true;
        }
        return false;
    }

    template<class U>
    [[nodiscard]] bool operator==(Rect<U> const& other) const
    {
        return location() == other.location() && size() == other.size();
    }

    [[nodiscard]] Rect<T> operator*(T factor) const { return { m_location * factor, m_size * factor }; }

    Rect<T>& operator*=(T factor)
    {
        m_location *= factor;
        m_size *= factor;
        return *this;
    }

    void intersect(Rect<T> const& other)
    {
        T l = max(left(), other.left());
        T r = min(right(), other.right());
        T t = max(top(), other.top());
        T b = min(bottom(), other.bottom());

        if (l > r || t > b) {
            m_location = {};
            m_size = {};
            return;
        }

        set_x(l);
        set_y(t);
        set_right(r);
        set_bottom(b);
    }

    [[nodiscard]] static Rect<T> centered_on(Point<T> const& center, Size<T> const& size)
    {
        return { { center.x() - size.width() / 2, center.y() - size.height() / 2 }, size };
    }

    [[nodiscard]] static Rect<T> from_two_points(Point<T> const& a, Point<T> const& b)
    {
        return { min(a.x(), b.x()), min(a.y(), b.y()), AK::abs(a.x() - b.x()), AK::abs(a.y() - b.y()) };
    }

    [[nodiscard]] static Rect<T> intersection(Rect<T> const& a, Rect<T> const& b)
    {
        Rect<T> r = a;
        r.intersect(b);
        return r;
    }

    [[nodiscard]] ALWAYS_INLINE Rect<T> intersected(Rect<T> const& other) const
    {
        return intersection(*this, other);
    }

    template<typename U = T>
    [[nodiscard]] Gfx::Rect<U> interpolated_to(Gfx::Rect<T> const& to, float factor) const
    {
        VERIFY(factor >= 0.f);
        VERIFY(factor <= 1.f);
        if (factor == 0.f)
            return *this;
        if (factor == 1.f)
            return to;
        if (this == &to)
            return *this;
        auto interpolated_left = round_to<U>(mix<float>(x(), to.x(), factor));
        auto interpolated_top = round_to<U>(mix<float>(y(), to.y(), factor));
        auto interpolated_right = round_to<U>(mix<float>(right(), to.right(), factor));
        auto interpolated_bottom = round_to<U>(mix<float>(bottom(), to.bottom(), factor));
        return { interpolated_left, interpolated_top, interpolated_right - interpolated_left, interpolated_bottom - interpolated_top };
    }

    [[nodiscard]] static Rect<T> centered_at(Point<T> const& point, Size<T> const& size)
    {
        return { { point.x() - size.width() / 2, point.y() - size.height() / 2 }, size };
    }

    void unite(Rect<T> const& other)
    {
        if (is_empty()) {
            *this = other;
            return;
        }
        if (other.is_empty())
            return;
        unite_horizontally(other);
        unite_vertically(other);
    }

    void unite_horizontally(Rect<T> const& other)
    {
        auto new_left = min(left(), other.left());
        auto new_right = max(right(), other.right());
        set_left(new_left);
        set_right(new_right);
    }

    void unite_vertically(Rect<T> const& other)
    {
        auto new_top = min(top(), other.top());
        auto new_bottom = max(bottom(), other.bottom());
        set_top(new_top);
        set_bottom(new_bottom);
    }

    [[nodiscard]] Rect<T> united(Rect<T> const& other) const
    {
        Rect<T> rect = *this;
        rect.unite(other);
        return rect;
    }

    [[nodiscard]] Point<T> top_left() const { return { left(), top() }; }
    [[nodiscard]] Point<T> top_right() const { return { right(), top() }; }
    [[nodiscard]] Point<T> bottom_left() const { return { left(), bottom() }; }
    [[nodiscard]] Point<T> bottom_right() const { return { right(), bottom() }; }

    void align_within(Rect<T> const& other, TextAlignment alignment)
    {
        switch (alignment) {
        case TextAlignment::Center:
            center_within(other);
            return;
        case TextAlignment::TopCenter:
            center_horizontally_within(other);
            set_y(other.y());
            return;
        case TextAlignment::TopLeft:
            set_location(other.location());
            return;
        case TextAlignment::TopRight:
            set_x(other.right() - width());
            set_y(other.y());
            return;
        case TextAlignment::CenterLeft:
            set_x(other.x());
            center_vertically_within(other);
            return;
        case TextAlignment::CenterRight:
            set_x(other.right() - width());
            center_vertically_within(other);
            return;
        case TextAlignment::BottomCenter:
            center_horizontally_within(other);
            set_y(other.bottom() - height());
            return;
        case TextAlignment::BottomLeft:
            set_x(other.x());
            set_y(other.bottom() - height());
            return;
        case TextAlignment::BottomRight:
            set_x(other.right() - width());
            set_y(other.bottom() - height());
            return;
        }
    }

    void center_within(Rect<T> const& other)
    {
        center_horizontally_within(other);
        center_vertically_within(other);
    }

    [[nodiscard]] Rect centered_within(Rect const& other) const
    {
        Rect rect { *this };
        rect.center_horizontally_within(other);
        rect.center_vertically_within(other);
        return rect;
    }

    void center_horizontally_within(Rect<T> const& other)
    {
        set_x(other.center().x() - width() / 2);
    }

    void center_vertically_within(Rect<T> const& other)
    {
        set_y(other.center().y() - height() / 2);
    }

    template<typename U>
    requires(!IsSame<T, U>)
    [[nodiscard]] ALWAYS_INLINE Rect<U> to_type() const
    {
        return Rect<U>(*this);
    }

    // For extern specialization, like CSSPixels
    template<typename U>
    [[nodiscard]] Rect<U> to_rounded() const = delete;

    template<FloatingPoint U>
    [[nodiscard]] ALWAYS_INLINE Rect<U> to_rounded() const
    {
        // FIXME: We may get away with `rint[lf]?()` here.
        //        This would even give us some more control of these internals,
        //        while the break-tie algorithm does not really matter
        if constexpr (IsSame<T, float>) {
            return {
                static_cast<U>(roundf(x())),
                static_cast<U>(roundf(y())),
                static_cast<U>(roundf(width())),
                static_cast<U>(roundf(height())),
            };
        }
        if constexpr (IsSame<T, double>) {
            return {
                static_cast<U>(round(x())),
                static_cast<U>(round(y())),
                static_cast<U>(round(width())),
                static_cast<U>(round(height())),
            };
        }

        return {
            static_cast<U>(roundl(x())),
            static_cast<U>(roundl(y())),
            static_cast<U>(roundl(width())),
            static_cast<U>(roundl(height())),
        };
    }

    template<Integral I>
    ALWAYS_INLINE Rect<I> to_rounded() const
    {
        return {
            round_to<I>(x()),
            round_to<I>(y()),
            round_to<I>(width()),
            round_to<I>(height()),
        };
    }

    [[nodiscard]] ByteString to_byte_string() const;

private:
    Point<T> m_location;
    Size<T> m_size;
};

using IntRect = Rect<int>;
using FloatRect = Rect<float>;
using DoubleRect = Rect<double>;

[[nodiscard]] ALWAYS_INLINE IntRect enclosing_int_rect(FloatRect const& float_rect)
{
    int x1 = floorf(float_rect.x());
    int y1 = floorf(float_rect.y());
    int x2 = ceilf(float_rect.right());
    int y2 = ceilf(float_rect.bottom());
    return Gfx::IntRect::from_two_points({ x1, y1 }, { x2, y2 });
}

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Rect<T>> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Rect<T> const& value)
    {
        return Formatter<FormatString>::format(builder, "[{},{} {}x{}]"sv, value.x(), value.y(), value.width(), value.height());
    }
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::IntRect const&);

template<>
ErrorOr<Gfx::IntRect> decode(Decoder&);

}
