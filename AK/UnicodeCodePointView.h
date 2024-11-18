/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AllOf.h>
#include <AK/AsciiChar.h>
#include <AK/Badge.h>
#include <AK/CharacterTypes.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRawPtr.h>
#include <AK/StringBuilder.h>
#include <AK/UnicodeCodePoint.h>
#include <AK/Vector.h>

namespace AK {

struct GlobMatchSpan {
    size_t start;
    size_t length;

    bool operator==(GlobMatchSpan const& other) const
    {
        return start == other.start && length == other.length;
    }
};

enum class AsciiCaseSensitivity {
    AsciiCaseInsensitive,
    AsciiCaseSensitive,
};

struct EmptyUnicodeCodePointIterableBase { };

class EmptyUnicodeCodePointView;
class SingleUnicodeCodePointView;
template<typename, typename, typename>
class UnicodeCodePointIterableBase;

template<typename T>
concept UnicodeCodePointIterable = IsTemplateBaseOf<UnicodeCodePointIterableBase, T>;

template<typename T>
concept UnicodeCodePointViewable = UnicodeCodePointIterable<T> || IsConstructible<UnicodeCodePoint, T>;

static inline SingleUnicodeCodePointView unicode_code_point_iterable(UnicodeCodePoint cp);

static inline UnicodeCodePointIterable auto const& unicode_code_point_iterable(UnicodeCodePointIterable auto const& view);

template<typename Self, typename ViewType, typename Base = EmptyUnicodeCodePointIterableBase>
class UnicodeCodePointIterableBase : public Base {
    AK_MAKE_CONDITIONALLY_COPYABLE(UnicodeCodePointIterableBase, <Base>);
    AK_MAKE_CONDITIONALLY_MOVABLE(UnicodeCodePointIterableBase, <Base>);

public:
    constexpr UnicodeCodePointIterableBase(Base const& other)
    requires(IsCopyConstructible<Base>)
        : Base(other)
    {
    }

    constexpr UnicodeCodePointIterableBase(Base&& other)
    requires(IsMoveConstructible<Base>)
        : Base(other)
    {
    }

    using Base::Base;

    ViewType unicode_code_point_view() const&
    {
        return static_cast<Self const&>(*this).unicode_code_point_view();
    }

    [[nodiscard]] UnicodeCodePointIterator<ViewType> begin() const&;
    [[nodiscard]] UnicodeCodePointIterator<ViewType> end() const&;

    [[nodiscard]] UnicodeCodePointIterator<ViewType> codepoints() const&;
    [[nodiscard]] UnicodeCodePointReversedIterator<ViewType> reversed_codepoints() const&;

    [[nodiscard]] size_t code_point_offset_of(UnicodeCodePointIterator<ViewType> const&) const;
    [[nodiscard]] size_t code_unit_offset_of(UnicodeCodePointIterator<ViewType> const&) const;

    [[nodiscard]] bool is_empty() const;
    [[nodiscard]] size_t length() const;

    [[nodiscard]] bool operator==(UnicodeCodePoint) const;
    [[nodiscard]] bool operator!=(UnicodeCodePoint other) const { return !(*this == other); }

    [[nodiscard]] bool operator==(UnicodeCodePointIterable auto const&) const;
    template<typename Other, typename OtherViewType, typename OtherBase = EmptyUnicodeCodePointIterableBase>
    requires((IsBaseOf<ViewType, OtherViewType> || IsBaseOf<OtherViewType, ViewType>) && !ViewType::is_lossy && !OtherViewType::is_lossy)
    [[nodiscard]] bool operator==(UnicodeCodePointIterableBase<Other, OtherViewType, OtherBase> const&) const;
    [[nodiscard]] bool operator!=(UnicodeCodePointIterable auto const& other) const { return !(*this == other); }

    template<typename T>
    ErrorOr<T> to() const
    {
        StringBuilder builder;
        for (auto cp : *this)
            TRY(builder.try_append_code_point(cp));
        return move(builder).to<T>();
    }

    template<typename T>
    ErrorOr<T> to_ascii_lowercase() const
    {
        StringBuilder builder;
        for (auto cp : *this)
            TRY(builder.try_append_code_point(AK::to_ascii_lowercase(cp)));
        return move(builder).to<T>();
    }

    template<typename T>
    ErrorOr<T> to_ascii_uppercase() const
    {
        StringBuilder builder;
        for (auto cp : *this)
            TRY(builder.try_append_code_point(AK::to_ascii_uppercase(cp)));
        return move(builder).to<T>();
    }

    template<typename T>
    ErrorOr<T> to_ascii_titlecase() const
    {
        StringBuilder builder;
        bool next_is_upper = true;

        for (auto cp : *this) {
            if (next_is_upper)
                builder.append(AK::to_ascii_uppercase(cp));
            else
                builder.append(AK::to_ascii_lowercase(cp));
            next_is_upper = cp == ' '_ascii;
        }

        return move(builder).to<T>();
    }

    template<typename T>
    ErrorOr<T> replace(UnicodeCodePointViewable auto const& needle, UnicodeCodePointViewable auto const& replacement) const
    {
        StringBuilder builder;

        auto remaining = *this;

        for (auto position = remaining.find(needle); position != remaining.end(); position = remaining.find(needle)) {
            builder.append(remaining.chomp_left(position));
            builder.append(replacement);
            remaining.chomp_left(needle.length());
        }
        builder.append(remaining);

        return move(builder).to<T>();
    }

    [[nodiscard]] ViewType unicode_substring_view(ssize_t start, ssize_t length) const&;
    [[nodiscard]] ViewType unicode_substring_view(ssize_t start, ssize_t length) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType unicode_substring_view(ssize_t start) const&;
    [[nodiscard]] ViewType unicode_substring_view(ssize_t start) && = delete;

    [[nodiscard]] ViewType unicode_substring_view(UnicodeCodePointIterator<ViewType> const&, ssize_t length) const&;
    [[nodiscard]] ViewType unicode_substring_view(UnicodeCodePointIterator<ViewType> const& position, ssize_t length) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType unicode_substring_view(UnicodeCodePointIterator<ViewType> const&) const&;
    [[nodiscard]] ViewType unicode_substring_view(UnicodeCodePointIterator<ViewType> const& position) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType prefix(size_t = 1) const&;
    [[nodiscard]] ViewType prefix(size_t n = 1) && = delete;

    [[nodiscard]] ViewType prefix(UnicodeCodePointIterator<ViewType> const&) const&;
    [[nodiscard]] ViewType prefix(UnicodeCodePointIterator<ViewType> const& position) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType suffix(size_t = 1) const&;
    [[nodiscard]] ViewType suffix(size_t n = 1) && = delete;

    [[nodiscard]] ViewType suffix(UnicodeCodePointIterator<ViewType> const&) const&;
    [[nodiscard]] ViewType suffix(UnicodeCodePointIterator<ViewType> const& position) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType trim(UnicodeCodePointViewable auto const&, TrimMode = TrimMode::Both) const&;
    [[nodiscard]] ViewType trim(UnicodeCodePointViewable auto const& characters, TrimMode mode = TrimMode::Both) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] ViewType trim_ascii_whitespace(TrimMode = TrimMode::Both) const&;
    [[nodiscard]] ViewType trim_ascii_whitespace(TrimMode mode = TrimMode::Both) &&
    requires(!SameAs<Self, ViewType>)
    = delete;

    [[nodiscard]] bool contains(auto const&, AsciiCaseSensitivity = AsciiCaseSensitivity::AsciiCaseSensitive) const;

    [[nodiscard]] bool starts_with(UnicodeCodePointViewable auto const&, AsciiCaseSensitivity = AsciiCaseSensitivity::AsciiCaseSensitive) const;

    [[nodiscard]] bool ends_with(UnicodeCodePointViewable auto const&, AsciiCaseSensitivity = AsciiCaseSensitivity::AsciiCaseSensitive) const;

    [[nodiscard]] bool equals(UnicodeCodePointViewable auto const& other, AsciiCaseSensitivity case_sensitivity = AsciiCaseSensitivity::AsciiCaseSensitive) const
    {
        if (case_sensitivity == AsciiCaseSensitivity::AsciiCaseInsensitive)
            return equals_ignoring_ascii_case(other);

        return *this == other;
    }

    [[nodiscard]] bool equals_ignoring_ascii_case(UnicodeCodePointViewable auto const&) const;

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (... || [this, &strings]() -> bool {
            return this->equals_ignoring_ascii_case(forward<Ts>(strings));
        }());
    }

    [[nodiscard]] bool is_ascii_whitespace() const;

    [[nodiscard]] size_t count(UnicodeCodePoint) const;
    [[nodiscard]] size_t count(UnicodeCodePointIterable auto const&) const;

    [[nodiscard]] UnicodeCodePointIterator<ViewType> find(UnicodeCodePoint) const;
    [[nodiscard]] UnicodeCodePointIterator<ViewType> find(UnicodeCodePointIterable auto const&) const;

    [[nodiscard]] UnicodeCodePointIterator<ViewType> find_last(UnicodeCodePoint) const;
    [[nodiscard]] UnicodeCodePointIterator<ViewType> find_last(UnicodeCodePointIterable auto const&) const;

    [[nodiscard]] bool matches_glob(UnicodeCodePointViewable auto const& glob, AsciiCaseSensitivity = AsciiCaseSensitivity::AsciiCaseSensitive, Vector<GlobMatchSpan>* = nullptr) const;

    ViewType& unicode_code_point_view_reference(ViewType&)
    requires(IsBaseOf<UnicodeCodePointIterableBase<Self, ViewType, Base>, ViewType>)
    {
        return static_cast<ViewType&>(*this);
    }

    ViewType& unicode_code_point_view_reference(ViewType& buffer)
    requires(!IsBaseOf<UnicodeCodePointIterableBase<Self, ViewType, Base>, ViewType>)
    {
        buffer = static_cast<Self&>(*this).unicode_code_point_view();
        return buffer;
    }

    ViewType const& unicode_code_point_view_reference(ViewType&) const
    requires(IsBaseOf<UnicodeCodePointIterableBase<Self, ViewType, Base>, ViewType>)
    {
        return static_cast<ViewType const&>(*this);
    }

    ViewType const& unicode_code_point_view_reference(ViewType& buffer) const
    requires(!IsBaseOf<UnicodeCodePointIterableBase<Self, ViewType, Base>, ViewType>)
    {
        buffer = static_cast<Self const&>(*this).unicode_code_point_view();
        return buffer;
    }
};

template<typename ViewType>
struct UnicodeCodePointSplitView {
    ViewType lhs;
    ViewType rhs;
};

template<typename Self>
class UnicodeCodePointView : public UnicodeCodePointIterableBase<Self, Self> {
    AK_MAKE_DEFAULT_COPYABLE(UnicodeCodePointView);
    AK_MAKE_DEFAULT_MOVABLE(UnicodeCodePointView);

public:
    ReadonlyBytes underlying_bytes() const = delete;

    Self unicode_code_point_view() const&
    {
        return static_cast<Self const&>(*this);
    }

    Self& unicode_code_point_view_reference(Self&)
    {
        return static_cast<Self&>(*this);
    }

    Self const& unicode_code_point_view_reference(Self&) const
    {
        return static_cast<Self const&>(*this);
    }

    bool is_empty() const
    {
        return m_code_unit_length == 0;
    }

    Optional<UnicodeCodePoint> chomp_one_left() & = delete;

    Optional<UnicodeCodePoint> chomp_one_left() && = delete;

    Optional<UnicodeCodePoint> chomp_one_right() & = delete;

    Optional<UnicodeCodePoint> chomp_one_right() && = delete;

    UnicodeCodePointSplitView<Self> split_at(UnicodeCodePointIterator<Self> const& position) const;

    size_t code_point_offset_of(UnicodeCodePointIterator<Self> const& iterator) const = delete;

    size_t code_unit_offset_of(UnicodeCodePointIterator<Self> const& iterator) const;

    Self chomp_left(size_t n = 1);

    Self chomp_left(UnicodeCodePointIterator<Self> const& position);

    Self chomp_right(size_t n = 1);

    Self chomp_right(UnicodeCodePointIterator<Self> const& position);

    Self chomp_left_while(auto const& predicate);

    Self chomp_right_while(auto const& predicate);

    UnicodeCodePointIterator<Self> begin() const;

    UnicodeCodePointIterator<Self> end() const;

    size_t calculate_length_in_code_points() const;

    size_t length() const
    {
        if (!m_code_point_length.has_value())
            m_code_point_length = calculate_length_in_code_points();
        return m_code_point_length.value();
    }

    Optional<size_t> length_without_side_effects() const
    {
        return m_code_point_length;
    }

protected:
    constexpr UnicodeCodePointView()
    {
    }

    constexpr UnicodeCodePointView(UnicodeCodePoint code_point)
        : m_code_point(code_point)
        , m_code_unit_length(1)
        , m_code_point_length(1)
    {
    }

    constexpr UnicodeCodePointView(void const* code_units, size_t code_unit_length, Optional<size_t> code_point_length = {})
        : m_code_units(code_units)
        , m_code_unit_length(code_unit_length)
        , m_code_point_length(code_point_length)
    {
    }

    union {
        UnicodeCodePoint m_code_point;
        void const* m_code_units { nullptr };
    };
    size_t m_code_unit_length { 0 };
    mutable Optional<size_t> m_code_point_length;
};

template<typename Self, typename CodeUnit, typename Base = UnicodeCodePointView<Self>>
class UnicodeCodePointViewBase : public Base {
public:
    constexpr UnicodeCodePointViewBase() = default;

    static constexpr bool is_lossy = true;

    ReadonlyBytes underlying_bytes() const
    {
        return to_readonly_bytes(code_units());
    }

    static constexpr Self from_span_unchecked(ReadonlySpan<char8_t> sv)
    {
        return Self { sv.data(), sv.size() };
    }

    static constexpr Self from_bytes_unchecked(ReadonlyBytes sv)
    {
        return Self { sv.data(), sv.size() };
    }

    static constexpr Self from_string_view_unchecked(StringView sv)
    {
        return Self::from_bytes_unchecked(sv.bytes());
    }

    Optional<UnicodeCodePoint> chomp_one_left() = delete;
    Optional<UnicodeCodePoint> chomp_one_right() = delete;

    ReadonlySpan<CodeUnit> code_units() const
    {
        return ReadonlySpan<CodeUnit> {
            static_cast<CodeUnit const*>(static_cast<Self const&>(*this).m_code_units),
            static_cast<Self const&>(*this).m_code_unit_length
        };
    }

    UnicodeCodePointIterator<Self> codepoints() const
    {
        return begin();
    }
    UnicodeCodePointReversedIterator<Self> reversed_codepoints() const
    {
        return begin().reversed();
    }

    Self unicode_code_point_view() const&
    {
        return static_cast<Self const&>(*this);
    }

    Self& unicode_code_point_view_reference(Self&)
    {
        return static_cast<Self&>(*this);
    }

    Self const& unicode_code_point_view_reference(Self&) const
    {
        return static_cast<Self const&>(*this);
    }

    bool is_empty() const
    {
        return static_cast<Self const&>(*this).m_code_unit_length == 0;
    }

    size_t length() const
    {
        auto const& self = static_cast<Self const&>(*this);

        if (!self.m_code_point_length.has_value())
            self.m_code_point_length = self.calculate_length_in_code_points();
        return self.m_code_point_length.value();
    }

    UnicodeCodePointSplitView<Self> split_at(UnicodeCodePointIterator<Self> const& position) const
    {
        auto const& self = static_cast<Self const&>(*this);

        size_t offset = self.code_unit_offset_of(position);
        Self lhs { self.m_code_units, offset };
        Self rhs { static_cast<u8 const*>(self.m_code_units) + offset, self.m_code_unit_length - offset };

        return UnicodeCodePointSplitView { lhs, rhs };
    }

    size_t code_point_offset_of(UnicodeCodePointIterator<Self> const& position) const
    {
        return codepoints().code_point_offset_of(position);
    }

    size_t code_unit_offset_of(UnicodeCodePointIterator<Self> const& position) const
    {
        auto const& self = static_cast<Self const&>(*this);

        return (reinterpret_cast<char const*>(position.m_view.m_code_units) - reinterpret_cast<char const*>(self.m_code_units)) / sizeof(CodeUnit);
    }

    Self chomp_left(size_t n = 1)
    {
        auto position = codepoints();
        while (n-- != 0)
            ++position;
        return chomp_left(position);
    }

    Self chomp_left(UnicodeCodePointIterator<Self> const& position)
    {
        auto [lhs, rhs] = split_at(position);
        *this = rhs;
        return lhs;
    }

    Self chomp_right(size_t n = 1)
    {
        auto position = reversed_codepoints();
        while (n-- != 0)
            ++position;
        return chomp_right(position);
    }

    Self chomp_right(UnicodeCodePointIterator<Self> const& position)
    {
        auto [lhs, rhs] = split_at(position);
        *this = lhs;
        return rhs;
    }

    Self chomp_left_while(auto const& predicate)
    {
        auto position = codepoints();
        while (!position.done() && predicate(*position))
            ++position;

        return chomp_left(position);
    }

    Self chomp_right_while(auto const& predicate)
    {
        auto position = reversed_codepoints();
        while (!position.done() && predicate(*position))
            ++position;

        return chomp_right(position);
    }

    UnicodeCodePointIterator<Self> begin() const
    {
        return UnicodeCodePointIterator(UnicodeCodePointIterator<Self>::Adopt, static_cast<Self const&>(*this));
    }

    UnicodeCodePointIterator<Self> end() const
    {
        auto const& self = static_cast<Self const&>(*this);

        return UnicodeCodePointIterator(
            UnicodeCodePointIterator<Self>::Adopt,
            Self {
                static_cast<u8 const*>(self.m_code_units) + self.m_code_unit_length * sizeof(CodeUnit),
                0,
                0,
            });
    }

protected:
    constexpr UnicodeCodePointViewBase(void const* code_units, size_t code_unit_length, Optional<size_t> code_point_length = {})
        : Base(code_units, code_unit_length, code_point_length)
    {
    }

    constexpr UnicodeCodePointViewBase(CodeUnit const* code_units, size_t code_unit_length, Optional<size_t> code_point_length = {})
        : Base(code_units, code_unit_length, code_point_length)
    {
    }

private:
    using Base::Base;
};

class EmptyUnicodeCodePointView : public UnicodeCodePointViewBase<EmptyUnicodeCodePointView, UnicodeCodePoint> {
public:
    EmptyUnicodeCodePointView() = default;

    static constexpr bool is_lossy = false;

    static Optional<UnicodeCodePoint> chomp_one_left()
    {
        return {};
    }

    static Optional<UnicodeCodePoint> chomp_one_right()
    {
        return {};
    }

    static UnicodeCodePointIterator<EmptyUnicodeCodePointView> end();
};

class SingleUnicodeCodePointView : public UnicodeCodePointViewBase<SingleUnicodeCodePointView, UnicodeCodePoint> {
public:
    SingleUnicodeCodePointView() = default;

    SingleUnicodeCodePointView(UnicodeCodePoint cp)
        : UnicodeCodePointViewBase(cp)
    {
    }

    static constexpr bool is_lossy = false;

    Optional<UnicodeCodePoint> chomp_one_left() &
    {
        if (m_code_unit_length == 0)
            return {};
        --m_code_unit_length;
        --*m_code_point_length;
        return m_code_point;
    }

    Optional<UnicodeCodePoint> chomp_one_right() &
    {
        if (m_code_unit_length == 0)
            return {};
        --m_code_unit_length;
        --*m_code_point_length;
        return m_code_point;
    }

    static UnicodeCodePointIterator<SingleUnicodeCodePointView> end();
};

template<typename Self>
size_t UnicodeCodePointView<Self>::calculate_length_in_code_points() const
{
    return static_cast<Self const&>(*this).code_point_offset_of(end());
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_left(size_t n)
{
    auto position = UnicodeCodePointIterableBase<Self, Self>::codepoints();
    while (n-- != 0)
        ++position;
    return chomp_left(position);
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_right(size_t n)
{
    auto position = UnicodeCodePointIterableBase<Self, Self>::reversed_codepoints();
    while (n-- != 0)
        ++position;
    return chomp_right(position);
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_left(UnicodeCodePointIterator<Self> const& position)
{
    auto [lhs, rhs] = split_at(position);
    *this = rhs;
    return lhs;
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_right(UnicodeCodePointIterator<Self> const& position)
{
    auto [lhs, rhs] = split_at(position);
    *this = lhs;
    return rhs;
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_left_while(auto const& predicate)
{
    auto position = UnicodeCodePointIterableBase<Self, Self>::codepoints();
    while (!position.done() && predicate(*position))
        ++position;

    return chomp_right(position);
}

template<typename Self>
Self UnicodeCodePointView<Self>::chomp_right_while(auto const& predicate)
{
    auto position = UnicodeCodePointIterableBase<Self, Self>::reversed_codepoints();
    while (!position.done() && predicate(*position))
        ++position;

    return chomp_right(position);
}

template<typename Self>
UnicodeCodePointIterator<Self> UnicodeCodePointView<Self>::begin() const
{
    return UnicodeCodePointIterator(UnicodeCodePointIterator<Self>::Adopt, static_cast<Self const&>(*this));
}

template<typename Self>
UnicodeCodePointIterator<Self> UnicodeCodePointView<Self>::end() const
{
    auto const& self = static_cast<Self const&>(*this);

    return self.end();
}

template<typename ViewType>
class UnicodeCodePointIterator : public UnicodeCodePointIterableBase<UnicodeCodePointIterator<ViewType>, ViewType> {
    AK_MAKE_DEFAULT_COPYABLE(UnicodeCodePointIterator);
    AK_MAKE_DEFAULT_MOVABLE(UnicodeCodePointIterator);

    template<typename OtherViewType>
    friend class UnicodeCodePointIterator;

public:
    enum AdoptTag { Adopt };

    UnicodeCodePointIterator() = delete;

    UnicodeCodePointIterator(AdoptTag, ViewType view)
        : m_view(move(view))
    {
    }

    template<typename OtherViewType>
    requires(IsBaseOf<ViewType, OtherViewType>)
    UnicodeCodePointIterator(UnicodeCodePointIterator<OtherViewType> const& other)
        : m_view(other.m_view)
    {
    }

    template<typename OtherViewType>
    requires(IsBaseOf<ViewType, OtherViewType>)
    UnicodeCodePointIterator(UnicodeCodePointIterator<OtherViewType>&& other)
        : m_view(move(other.m_view))
    {
    }

    explicit consteval UnicodeCodePointIterator(UnicodeCodePoint codepoint)
        : m_view(codepoint)
    {
    }

    explicit consteval UnicodeCodePointIterator(AsciiChar codepoint)
        : UnicodeCodePointIterator(UnicodeCodePoint(codepoint))
    {
    }

    ViewType unicode_code_point_view() const&
    {
        return m_view;
    }

    ViewType& unicode_code_point_view_reference(ViewType&)
    {
        return m_view;
    }

    ViewType const& unicode_code_point_view_reference(ViewType&) const
    {
        return m_view;
    }

    UnicodeCodePoint operator*() const
    {
        auto copy = ViewType(m_view);
        return copy.chomp_one_left().release_value();
    }

    inline UnicodeCodePointIterator& operator++()
    {
        (void)m_view.chomp_one_left();
        return *this;
    }

    inline UnicodeCodePointIterator operator++(int)
    {
        UnicodeCodePointIterator old = *this;
        ++*this;
        return old;
    }

    UnicodeCodePointIterator& shrink_one()
    {
        (void)m_view.chomp_one_right();
        return *this;
    }

    UnicodeCodePointIterator& advance(size_t n = 1)
    {
        m_view.chomp_left(n);
        return *this;
    }

    UnicodeCodePointIterator& shrink(size_t n = 1)
    {
        m_view.chomp_right(n);
        return *this;
    }

    bool operator==(UnicodeCodePointIterator const& other) const
    {
        if (m_view.is_empty() || other.m_view.is_empty())
            return m_view.is_empty() && other.m_view.is_empty();

        if (m_view.code_unit_offset_of(end()) != other.m_view.code_unit_offset_of(end()))
            return false;

        return m_view.underlying_bytes().data() == other.m_view.underlying_bytes().data();
    }

    inline bool operator!=(UnicodeCodePointIterator const& other) const
    {
        return !(*this == other);
    }

    bool have(size_t n)
    {
        auto iter = *this;

        while (n-- != 0)
            ++iter;

        return !iter.done();
    }

    bool done() const
    {
        return *this == end();
    }

    UnicodeCodePointIterator begin() const
    {
        return *this;
    }
    UnicodeCodePointIterator end() const
    {
        ViewType buffer;
        return unicode_code_point_view_reference(buffer).end();
    }

    ssize_t code_point_offset_of(UnicodeCodePointIterator const& other) const
    {
        auto lhs = *this;
        auto rhs = other;

        ssize_t offset = 0;
        if (lhs.code_unit_offset_of(rhs) >= 0) {
            while (lhs != rhs) {
                ++lhs;
                ++offset;
            }
        } else {
            while (lhs != rhs) {
                ++rhs;
                --offset;
            }
        }

        return offset;
    }

    ssize_t code_unit_offset_of(UnicodeCodePointIterator const& other) const
    {
        if (other.done())
            return static_cast<ssize_t>(m_view.code_unit_offset_of(m_view.end()));
        if (other.m_view.underlying_bytes().data() < m_view.underlying_bytes().data())
            return -static_cast<ssize_t>(other.m_view.code_unit_offset_of(*this));
        return static_cast<ssize_t>(m_view.code_unit_offset_of(other));
    }

    UnicodeCodePointReversedIterator<ViewType> reversed() const;

private:
    friend struct UnicodeCodePointViewVirtualTable;
    template<typename, typename, typename>
    friend class UnicodeCodePointViewBase;

    ViewType m_view;
};

template<typename ViewType>
class UnicodeCodePointReversedIterator {
    AK_MAKE_DEFAULT_COPYABLE(UnicodeCodePointReversedIterator);
    AK_MAKE_DEFAULT_MOVABLE(UnicodeCodePointReversedIterator);

private:
    explicit UnicodeCodePointReversedIterator(UnicodeCodePointIterator<ViewType> iter, ViewType original)
        : m_iter(move(iter))
        , m_original(move(original))
    {
    }

public:
    UnicodeCodePointReversedIterator() = delete;

    explicit UnicodeCodePointReversedIterator(UnicodeCodePointIterator<ViewType> const& iter)
        : m_iter(iter)
        , m_original(iter.unicode_code_point_view())
    {
    }

    inline operator UnicodeCodePointIterator<ViewType>() const
    {
        return unreversed();
    }

    inline UnicodeCodePointIterator<ViewType> unreversed() const
    {
        auto view = ViewType(m_original);
        view.chomp_left(m_iter.end());
        return view.codepoints();
    }

    inline UnicodeCodePointReversedIterator const& begin() const
    {
        return *this;
    }

    inline UnicodeCodePointReversedIterator end() const
    {
        auto copy = m_iter.unicode_code_point_view();
        return copy.chomp_left(0).reversed_codepoints();
    }

    UnicodeCodePoint operator*() const
    {
        auto copy = m_iter.unicode_code_point_view();
        return copy.chomp_one_right().release_value();
    }

    inline UnicodeCodePointReversedIterator& operator++()
    {
        m_iter.shrink_one();
        return *this;
    }

    inline UnicodeCodePointReversedIterator operator++(int)
    {
        UnicodeCodePointReversedIterator old = *this;
        ++*this;
        return old;
    }

    bool operator==(UnicodeCodePointReversedIterator const& other) const
    {
        ViewType buffer;
        return m_iter == other.m_iter
            && m_iter.unicode_code_point_view_reference(buffer).code_point_offset_of(m_iter) == other.m_iter.unicode_code_point_view_reference(buffer).code_point_offset_of(m_iter);
    }

    inline bool operator!=(UnicodeCodePointReversedIterator const& other) const
    {
        return !(*this == other);
    }

    bool have(size_t n)
    {
        auto iter = *this;

        while (n-- != 0)
            ++iter;

        return !iter.done();
    }

    inline bool done() const
    {
        return *this == end();
    }

private:
    UnicodeCodePointIterator<ViewType> m_iter;
    ViewType m_original;
};

inline UnicodeCodePointIterator<EmptyUnicodeCodePointView> EmptyUnicodeCodePointView::end()
{
    return UnicodeCodePointIterator(
        UnicodeCodePointIterator<EmptyUnicodeCodePointView>::Adopt,
        EmptyUnicodeCodePointView {});
}

inline UnicodeCodePointIterator<SingleUnicodeCodePointView> SingleUnicodeCodePointView::end()
{
    return UnicodeCodePointIterator(
        UnicodeCodePointIterator<SingleUnicodeCodePointView>::Adopt,
        SingleUnicodeCodePointView {});
}

template<typename ViewType>
inline UnicodeCodePointReversedIterator<ViewType> UnicodeCodePointIterator<ViewType>::reversed() const
{
    return UnicodeCodePointReversedIterator(*this);
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::begin() const&
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).begin();
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::end() const&
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).end();
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::codepoints() const&
{
    return begin();
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointReversedIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::reversed_codepoints() const&
{
    return begin().reversed();
}

template<typename Self, typename ViewType, typename Base>
inline size_t UnicodeCodePointIterableBase<Self, ViewType, Base>::code_point_offset_of(UnicodeCodePointIterator<ViewType> const& position) const
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).code_point_offset_of(position);
}

template<typename Self, typename ViewType, typename Base>
inline size_t UnicodeCodePointIterableBase<Self, ViewType, Base>::code_unit_offset_of(UnicodeCodePointIterator<ViewType> const& position) const
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).code_unit_offset_of(position);
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::is_empty() const
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).is_empty();
}

template<typename Self, typename ViewType, typename Base>
inline size_t UnicodeCodePointIterableBase<Self, ViewType, Base>::length() const
{
    ViewType buffer;
    return unicode_code_point_view_reference(buffer).length();
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::operator==(UnicodeCodePoint other) const
{
    auto iter = begin();

    if (iter.done())
        return false;
    if (*iter != other)
        return false;
    ++iter;

    return iter.done();
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::operator==(UnicodeCodePointIterable auto const& other) const
{
    auto lhs = begin();
    auto rhs = other.begin();

    while (!lhs.done() && !rhs.done()) {
        if (*lhs != *rhs)
            return false;
        ++lhs;
        ++rhs;
    }

    return lhs.done() && rhs.done();
}

template<typename Self, typename ViewType, typename Base>
template<typename Other, typename OtherViewType, typename OtherBase>
requires((IsBaseOf<ViewType, OtherViewType> || IsBaseOf<OtherViewType, ViewType>) && !ViewType::is_lossy && !OtherViewType::is_lossy)
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::operator==(UnicodeCodePointIterableBase<Other, OtherViewType, OtherBase> const& other) const
{
    // OPTIMIZATION: If the views have compatible encodings, and round-trip to UTF-32 losslessly, then we can compare bitwise.

    ViewType buffer;
    OtherViewType other_buffer;

    return static_cast<Self const&>(*this).unicode_code_point_view_reference(buffer).underlying_bytes() == static_cast<Other const&>(other).unicode_code_point_view_reference(other_buffer).underlying_bytes();
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::unicode_substring_view(ssize_t start, ssize_t length) const&
{
    ViewType buffer;
    auto result = unicode_code_point_view_reference(buffer);

    if (start < 0)
        result = result.chomp_right(-start + 1);
    else
        result.chomp_left(start);

    if (length >= 0)
        return result.chomp_left(length);
    result.chomp_right(-length + 1);

    return result;
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::unicode_substring_view(ssize_t start) const&
{
    ViewType buffer;
    auto result = unicode_code_point_view_reference(buffer);

    if (start < 0)
        return result.chomp_right(-start + 1);
    result.chomp_left(start);

    return result;
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::unicode_substring_view(UnicodeCodePointIterator<ViewType> const& position, ssize_t length) const&
{
    ViewType buffer;
    auto result = unicode_code_point_view_reference(buffer);
    result.chomp_left(position);
    if (length >= 0)
        return result.chomp_left(length);
    result.chomp_right(-length + 1);
    return result;
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::unicode_substring_view(UnicodeCodePointIterator<ViewType> const& position) const&
{
    ViewType buffer;
    auto result = unicode_code_point_view_reference(buffer);
    result.chomp_left(position);
    return result;
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::prefix(size_t n) const&
{
    return static_cast<Self const&>(*this).unicode_code_point_view().chomp_left(n);
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::prefix(UnicodeCodePointIterator<ViewType> const& position) const&
{
    return static_cast<Self const&>(*this).unicode_code_point_view().chomp_left(position);
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::suffix(size_t n) const&
{
    return static_cast<Self const&>(*this).unicode_code_point_view().chomp_right(n);
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::suffix(UnicodeCodePointIterator<ViewType> const& position) const&
{
    return static_cast<Self const&>(*this).unicode_code_point_view().chomp_right(position);
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::trim(UnicodeCodePointViewable auto const& characters_, TrimMode mode) const&
{
    auto const& characters = unicode_code_point_iterable(characters_);

    auto start = codepoints();
    if (mode != TrimMode::Right) {
        while (!start.done() && characters.contains(*start)) {
            ++start;
        }
    }

    auto end = reversed_codepoints();
    if (mode != TrimMode::Left) {
        while (!end.done() && characters.contains(*end)) {
            ++end;
        }
    }

    return static_cast<Self const&>(*this).unicode_code_point_view().chomp_left(end).chomp_right(start);
}

template<typename Self, typename ViewType, typename Base>
inline ViewType UnicodeCodePointIterableBase<Self, ViewType, Base>::trim_ascii_whitespace(TrimMode mode) const&
{
    return trim(ViewType::from_string_view_unchecked(" \n\t\v\f\r"sv), mode);
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::contains(auto const& needle, AsciiCaseSensitivity case_sensitivity) const
{
    auto iter = begin();

    while (!iter.done()) {
        if (iter.starts_with(needle, case_sensitivity))
            return true;
        ++iter;
    }

    return false;
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::starts_with(UnicodeCodePointViewable auto const& needle_, AsciiCaseSensitivity case_sensitivity) const
{
    auto const& needle = unicode_code_point_iterable(needle_);
    return prefix(needle.length()).equals(needle, case_sensitivity);
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::ends_with(UnicodeCodePointViewable auto const& needle_, AsciiCaseSensitivity case_sensitivity) const
{
    auto const& needle = unicode_code_point_iterable(needle_);
    return suffix(needle.length()).equals(needle, case_sensitivity);
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::equals_ignoring_ascii_case(UnicodeCodePointViewable auto const& other) const
{
    auto iter = begin();

    for (auto cp : unicode_code_point_iterable(other)) {
        if (iter.done() || AK::to_ascii_lowercase(cp) != AK::to_ascii_lowercase(*iter))
            return false;
    }

    return true;
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::is_ascii_whitespace() const
{
    return all_of(*this, is_ascii_space);
}

template<typename Self, typename ViewType, typename Base>
inline size_t UnicodeCodePointIterableBase<Self, ViewType, Base>::count(UnicodeCodePoint needle) const
{
    size_t count = 0;
    for (auto cp : *this) {
        if (cp == needle)
            count++;
    }
    return count;
}

template<typename Self, typename ViewType, typename Base>
inline size_t UnicodeCodePointIterableBase<Self, ViewType, Base>::count(UnicodeCodePointIterable auto const& needle) const
{
    if (needle.is_empty())
        return length();

    size_t count = 0;
    for (auto iter = begin(); iter != end(); ++iter) {
        if (iter.starts_with(needle))
            count++;
    }
    return count;
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::find(UnicodeCodePoint needle) const
{
    auto iter = begin();

    while (!iter.done()) {
        if (*iter == needle)
            return iter;
        ++iter;
    }

    return iter;
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::find(UnicodeCodePointIterable auto const& needle) const
{
    auto iter = begin();

    while (!iter.done()) {
        if (iter.starts_with(needle))
            return iter;
        ++iter;
    }

    return iter;
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::find_last(UnicodeCodePoint needle) const
{
    auto iter = reversed_codepoints();

    while (!iter.done()) {
        if (*iter++ == needle)
            return iter;
    }

    return iter;
}

template<typename Self, typename ViewType, typename Base>
inline UnicodeCodePointIterator<ViewType> UnicodeCodePointIterableBase<Self, ViewType, Base>::find_last(UnicodeCodePointIterable auto const& needle) const
{
    auto iter = reversed_codepoints();

    while (!iter.done()) {
        if (iter.unreversed().starts_with(needle))
            return iter;
        ++iter;
    }

    return iter;
}

template<typename Self, typename ViewType, typename Base>
inline bool UnicodeCodePointIterableBase<Self, ViewType, Base>::matches_glob(UnicodeCodePointViewable auto const& glob_, AsciiCaseSensitivity case_sensitivity, Vector<GlobMatchSpan>* match_spans) const
{
    auto const& glob = unicode_code_point_iterable(glob_);

    if (is_empty() || glob.is_empty())
        return is_empty() && glob.is_empty();

    if (glob == '*'_ascii) {
        if (match_spans)
            match_spans->append({ 0, length() });
        return true;
    }

    auto string_ptr = begin();
    auto glob_ptr = glob.begin();

    while (!string_ptr.done() && !glob_ptr.done()) {
        switch (*glob_ptr) {
        case '*'_ascii:
            if (!glob_ptr.have(1)) {
                if (match_spans)
                    match_spans->append({ code_point_offset_of(string_ptr), string_ptr.length() });
                return true;
            } else {
                auto string_start_ptr = string_ptr;
                auto prev_string_ptr = string_ptr;
                while (!string_ptr.done() && !string_ptr.matches_glob(glob_ptr.unicode_substring_view(1), case_sensitivity))
                    prev_string_ptr = string_ptr++;
                if (match_spans)
                    match_spans->append({ code_point_offset_of(string_start_ptr), static_cast<size_t>(string_start_ptr.code_point_offset_of(string_ptr)) });
                string_ptr = prev_string_ptr;
            }
            break;
        case '?'_ascii:
            if (match_spans)
                match_spans->append({ code_point_offset_of(string_ptr), 1 });
            break;
        case '\\'_ascii:
            // if backslash is last character in mask, just treat it as an exact match
            // otherwise use it as escape for next character
            if (glob_ptr.have(1))
                ++glob_ptr;
            [[fallthrough]];
        default:
            auto p = *glob_ptr;
            auto ch = *string_ptr;
            if (case_sensitivity == AsciiCaseSensitivity::AsciiCaseSensitive ? p != ch : AK::to_ascii_lowercase(p) != AK::to_ascii_lowercase(ch))
                return false;
            break;
        }
        ++string_ptr;
        ++glob_ptr;
    }

    if (string_ptr.done()) {
        // Allow ending '*' to contain nothing.
        while (!glob_ptr.done() && *glob_ptr == '*'_ascii) {
            if (match_spans)
                match_spans->append({ code_point_offset_of(string_ptr), 0 });
            ++glob_ptr;
        }
    }

    return string_ptr.done() && glob_ptr.done();
}

static inline SingleUnicodeCodePointView unicode_code_point_iterable(UnicodeCodePoint cp)
{
    return SingleUnicodeCodePointView { cp };
}

static inline UnicodeCodePointIterable auto const& unicode_code_point_iterable(UnicodeCodePointIterable auto const& view)
{
    return view;
}

}
