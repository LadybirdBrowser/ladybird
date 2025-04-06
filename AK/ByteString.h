/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteStringImpl.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/RefPtr.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/Traits.h>

namespace AK {

// ByteString is a convenience wrapper around StringImpl, suitable for passing
// around as a value type. It's basically the same as passing around a
// RefPtr<StringImpl const>, with a bit of syntactic sugar.
//
// Note that StringImpl is an immutable object that cannot shrink or grow.
// Its allocation size is snugly tailored to the specific string it contains.
// Copying a ByteString is very efficient, since the internal StringImpl is
// retainable and so copying only requires modifying the ref count.
//
// There are three main ways to construct a new ByteString:
//
//     s = ByteString("some literal");
//
//     s = ByteString::formatted("{} little piggies", m_piggies);
//
//     StringBuilder builder;
//     builder.append("abc");
//     builder.append("123");
//     s = builder.to_byte_string();

class ByteString {
public:
    ~ByteString() = default;

    ByteString()
        : m_impl(ByteStringImpl::the_empty_stringimpl())
    {
    }

    ByteString(StringView view)
        : m_impl(ByteStringImpl::create(view.characters_without_null_termination(), view.length()))
    {
    }

    ByteString(ByteString const& other)
        : m_impl(other.m_impl)
    {
    }

    ByteString(ByteString&& other)
        : m_impl(move(other.m_impl))
    {
        other.m_impl = ByteStringImpl::the_empty_stringimpl();
    }

    ByteString(char const* cstring, ShouldChomp shouldChomp = NoChomp)
        : m_impl(ByteStringImpl::create(cstring, shouldChomp))
    {
    }

    ByteString(char const* cstring, size_t length, ShouldChomp shouldChomp = NoChomp)
        : m_impl(ByteStringImpl::create(cstring, length, shouldChomp))
    {
    }

    explicit ByteString(ReadonlyBytes bytes, ShouldChomp shouldChomp = NoChomp)
        : m_impl(ByteStringImpl::create(bytes, shouldChomp))
    {
    }

    ByteString(ByteStringImpl const& impl)
        : m_impl(impl)
    {
    }

    ByteString(NonnullRefPtr<ByteStringImpl const>&& impl)
        : m_impl(move(impl))
    {
    }

    ByteString(FlyString const&);

    template<
        typename F,
        typename PossiblyErrorOr = decltype(declval<F>()(declval<Bytes>())),
        bool is_error_or = IsSpecializationOf<PossiblyErrorOr, ErrorOr>,
        typename ReturnType = Conditional<is_error_or, ErrorOr<ByteString>, ByteString>>
    static ReturnType create_and_overwrite(size_t length, F&& fill_function)
    {
        char* buffer;
        auto impl = ByteStringImpl::create_uninitialized(length, buffer);

        if constexpr (is_error_or)
            TRY(fill_function(Bytes { buffer, length }));
        else
            fill_function(Bytes { buffer, length });
        return impl;
    }

    [[nodiscard]] static ByteString repeated(char, size_t count);
    [[nodiscard]] static ByteString repeated(StringView, size_t count);

    template<class SeparatorType, class CollectionType>
    [[nodiscard]] static ByteString join(SeparatorType const& separator, CollectionType const& collection, StringView fmtstr = "{}"sv)
    {
        StringBuilder builder;
        builder.join(separator, collection, fmtstr);
        return builder.to_byte_string();
    }

    [[nodiscard]] bool matches(StringView mask, CaseSensitivity = CaseSensitivity::CaseInsensitive) const;
    [[nodiscard]] bool matches(StringView mask, Vector<MaskSpan>&, CaseSensitivity = CaseSensitivity::CaseInsensitive) const;

    template<Arithmetic T>
    [[nodiscard]] static ByteString number(T value)
    {
        return formatted("{}", value);
    }

    template<Arithmetic T>
    Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes) const
    {
        return view().to_number<T>(trim_whitespace);
    }

    [[nodiscard]] ByteString to_lowercase() const;
    [[nodiscard]] ByteString to_uppercase() const;
    [[nodiscard]] ByteString to_snakecase() const;

    [[nodiscard]] bool is_whitespace() const { return StringUtils::is_whitespace(*this); }

    [[nodiscard]] ByteString trim(StringView characters, TrimMode mode = TrimMode::Both) const
    {
        auto trimmed_view = StringUtils::trim(view(), characters, mode);
        if (view() == trimmed_view)
            return *this;
        return trimmed_view;
    }

    [[nodiscard]] ByteString trim_whitespace(TrimMode mode = TrimMode::Both) const
    {
        auto trimmed_view = StringUtils::trim_whitespace(view(), mode);
        if (view() == trimmed_view)
            return *this;
        return trimmed_view;
    }

    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;

    [[nodiscard]] bool contains(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool contains(char, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] Vector<ByteString> split_limit(char separator, size_t limit, SplitBehavior = SplitBehavior::Nothing) const;
    [[nodiscard]] Vector<ByteString> split(char separator, SplitBehavior = SplitBehavior::Nothing) const;

    [[nodiscard]] Vector<StringView> split_view(char separator, SplitBehavior = SplitBehavior::Nothing) const&;
    [[nodiscard]] Vector<StringView> split_view(char separator, SplitBehavior = SplitBehavior::Nothing) const&& = delete;

    [[nodiscard]] Vector<StringView> split_view(Function<bool(char)> separator, SplitBehavior = SplitBehavior::Nothing) const&;
    [[nodiscard]] Vector<StringView> split_view(Function<bool(char)> separator, SplitBehavior = SplitBehavior::Nothing) const&& = delete;

    [[nodiscard]] Optional<size_t> find(char needle, size_t start = 0) const { return StringUtils::find(*this, needle, start); }
    [[nodiscard]] Optional<size_t> find(StringView needle, size_t start = 0) const { return StringUtils::find(*this, needle, start); }
    [[nodiscard]] Optional<size_t> find_last(char needle) const { return StringUtils::find_last(*this, needle); }
    [[nodiscard]] Optional<size_t> find_last(StringView needle) const { return StringUtils::find_last(*this, needle); }
    Vector<size_t> find_all(StringView needle) const;
    using SearchDirection = StringUtils::SearchDirection;
    [[nodiscard]] Optional<size_t> find_any_of(StringView needles, SearchDirection direction = SearchDirection::Forward) const { return StringUtils::find_any_of(*this, needles, direction); }

    [[nodiscard]] StringView find_last_split_view(char separator) const& { return view().find_last_split_view(separator); }
    [[nodiscard]] StringView find_last_split_view(char separator) const&& = delete;

    [[nodiscard]] ByteString substring(size_t start, size_t length) const;
    [[nodiscard]] ByteString substring(size_t start) const;

    [[nodiscard]] StringView substring_view(size_t start, size_t length) const&;
    [[nodiscard]] StringView substring_view(size_t start, size_t length) const&& = delete;

    [[nodiscard]] StringView substring_view(size_t start) const&;
    [[nodiscard]] StringView substring_view(size_t start) const&& = delete;

    [[nodiscard]] ALWAYS_INLINE bool is_empty() const { return length() == 0; }
    [[nodiscard]] ALWAYS_INLINE size_t length() const { return m_impl->length(); }
    // Includes NUL-terminator.
    [[nodiscard]] ALWAYS_INLINE char const* characters() const { return m_impl->characters(); }

    [[nodiscard]] bool copy_characters_to_buffer(char* buffer, size_t buffer_size) const;

    [[nodiscard]] ALWAYS_INLINE ReadonlyBytes bytes() const& { return m_impl->bytes(); }
    [[nodiscard]] ALWAYS_INLINE ReadonlyBytes bytes() const&& = delete;

    [[nodiscard]] ALWAYS_INLINE char const& operator[](size_t i) const
    {
        return (*m_impl)[i];
    }

    [[nodiscard]] ALWAYS_INLINE u8 byte_at(size_t i) const
    {
        return bit_cast<u8>((*m_impl)[i]);
    }

    using ConstIterator = SimpleIterator<ByteString const, char const>;

    [[nodiscard]] constexpr ConstIterator begin() const { return ConstIterator::begin(*this); }
    [[nodiscard]] constexpr ConstIterator end() const { return ConstIterator::end(*this); }

    [[nodiscard]] bool starts_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool ends_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool starts_with(char) const;
    [[nodiscard]] bool ends_with(char) const;

    bool operator==(ByteString const&) const;

    bool operator==(StringView) const;

    bool operator<(ByteString const&) const;
    bool operator>=(ByteString const& other) const { return !(*this < other); }
    bool operator>=(char const* other) const { return !(*this < other); }

    bool operator>(ByteString const&) const;
    bool operator<=(ByteString const& other) const { return !(*this > other); }
    bool operator<=(char const* other) const { return !(*this > other); }

    bool operator==(char const* cstring) const;

    [[nodiscard]] static ByteString empty()
    {
        return ByteStringImpl::the_empty_stringimpl();
    }

    [[nodiscard]] NonnullRefPtr<ByteStringImpl const> impl() const { return m_impl; }

    ByteString& operator=(ByteString&& other)
    {
        if (this != &other)
            m_impl = move(other.m_impl);
        return *this;
    }

    ByteString& operator=(ByteString const& other)
    {
        if (this != &other)
            m_impl = const_cast<ByteString&>(other).m_impl;
        return *this;
    }

    template<OneOf<ReadonlyBytes, Bytes> T>
    ByteString& operator=(T bytes)
    {
        m_impl = ByteStringImpl::create(bytes);
        return *this;
    }

    [[nodiscard]] u32 hash() const
    {
        return m_impl->hash();
    }

    [[nodiscard]] ByteBuffer to_byte_buffer() const;

    template<typename BufferType>
    [[nodiscard]] static ByteString copy(BufferType const& buffer, ShouldChomp should_chomp = NoChomp)
    {
        if (buffer.is_empty())
            return empty();
        return ByteString(reinterpret_cast<char const*>(buffer.data()), buffer.size(), should_chomp);
    }

    [[nodiscard]] static ByteString vformatted(StringView fmtstr, TypeErasedFormatParams&);

    template<typename... Parameters>
    [[nodiscard]] static ByteString formatted(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        return vformatted(fmtstr.view(), variadic_format_parameters);
    }

    [[nodiscard]] StringView view() const& { return { characters(), length() }; }
    [[nodiscard]] StringView view() const&& = delete;

    [[nodiscard]] ByteString replace(StringView needle, StringView replacement, ReplaceMode replace_mode = ReplaceMode::All) const { return StringUtils::replace(*this, needle, replacement, replace_mode); }
    [[nodiscard]] size_t count(StringView needle) const { return StringUtils::count(*this, needle); }
    [[nodiscard]] ByteString reverse() const;

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (... ||
                [this, &strings]() -> bool {
            if constexpr (requires(Ts a) { a.view()->StringView; })
                return this->equals_ignoring_ascii_case(forward<Ts>(strings.view()));
            else
                return this->equals_ignoring_ascii_case(forward<Ts>(strings));
        }());
    }

private:
    NonnullRefPtr<ByteStringImpl const> m_impl;
};

template<>
struct Traits<ByteString> : public DefaultTraits<ByteString> {
    static unsigned hash(ByteString const& s) { return s.impl()->hash(); }
};

// FIXME: Rename this to indicate that it's about ASCII-only case insensitivity.
struct CaseInsensitiveStringTraits : public Traits<ByteString> {
    static unsigned hash(ByteString const& s) { return s.impl()->case_insensitive_hash(); }
    static bool equals(ByteString const& a, ByteString const& b) { return a.equals_ignoring_ascii_case(b); }
};

ByteString escape_html_entities(StringView html);

}

#if USING_AK_GLOBALLY
using AK::CaseInsensitiveStringTraits;
using AK::escape_html_entities;
#endif
