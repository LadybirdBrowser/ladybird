/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringUtils.h>

namespace AK {

class DeprecatedFlyString {
public:
    DeprecatedFlyString()
        : m_impl(StringImpl::the_empty_stringimpl())
    {
    }
    DeprecatedFlyString(DeprecatedFlyString const& other)
        : m_impl(other.impl())
    {
    }
    DeprecatedFlyString(DeprecatedFlyString&& other)
        : m_impl(move(other.m_impl))
    {
    }
    DeprecatedFlyString(ByteString const&);
    DeprecatedFlyString(StringView);
    DeprecatedFlyString(char const* string)
        : DeprecatedFlyString(static_cast<ByteString>(string))
    {
    }

    static DeprecatedFlyString from_fly_impl(NonnullRefPtr<StringImpl const> impl)
    {
        VERIFY(impl->is_fly());
        DeprecatedFlyString string;
        string.m_impl = move(impl);
        return string;
    }

    DeprecatedFlyString& operator=(DeprecatedFlyString const& other)
    {
        m_impl = other.m_impl;
        return *this;
    }

    DeprecatedFlyString& operator=(DeprecatedFlyString&& other)
    {
        m_impl = move(other.m_impl);
        return *this;
    }

    bool is_empty() const { return !m_impl->length(); }

    bool operator==(DeprecatedFlyString const& other) const { return m_impl == other.m_impl; }

    bool operator==(ByteString const&) const;

    bool operator==(StringView) const;

    bool operator==(char const*) const;

    NonnullRefPtr<StringImpl const> impl() const { return m_impl; }
    char const* characters() const { return m_impl->characters(); }
    size_t length() const { return m_impl->length(); }

    ALWAYS_INLINE u32 hash() const { return m_impl->existing_hash(); }
    ALWAYS_INLINE StringView view() const { return m_impl->view(); }

    DeprecatedFlyString to_lowercase() const;

    template<Arithmetic T>
    Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes) const
    {
        return view().to_number<T>(trim_whitespace);
    }

    bool equals_ignoring_ascii_case(StringView) const;
    bool starts_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    bool ends_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    static void did_destroy_impl(Badge<StringImpl>, StringImpl&);

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

private:
    NonnullRefPtr<StringImpl const> m_impl;
};

template<>
struct Traits<DeprecatedFlyString> : public DefaultTraits<DeprecatedFlyString> {
    static unsigned hash(DeprecatedFlyString const& s) { return s.hash(); }
};

}

#if USING_AK_GLOBALLY
using AK::DeprecatedFlyString;
#endif
