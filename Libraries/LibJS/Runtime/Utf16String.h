/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Completion.h>

namespace JS {
namespace Detail {

class Utf16StringImpl : public RefCounted<Utf16StringImpl> {
public:
    ~Utf16StringImpl() = default;

    [[nodiscard]] static NonnullRefPtr<Utf16StringImpl> create();
    [[nodiscard]] static NonnullRefPtr<Utf16StringImpl> create(Utf16Data);
    [[nodiscard]] static NonnullRefPtr<Utf16StringImpl> create(StringView);
    [[nodiscard]] static NonnullRefPtr<Utf16StringImpl> create(Utf16View const&);

    Utf16Data const& string() const;
    Utf16View view() const;

    [[nodiscard]] u32 hash() const
    {
        if (!m_has_hash) {
            m_hash = compute_hash();
            m_has_hash = true;
        }
        return m_hash;
    }
    [[nodiscard]] bool operator==(Utf16StringImpl const& other) const { return string() == other.string(); }

private:
    Utf16StringImpl() = default;
    explicit Utf16StringImpl(Utf16Data string);

    [[nodiscard]] u32 compute_hash() const;

    mutable bool m_has_hash { false };
    mutable u32 m_hash { 0 };
    Utf16Data m_string;
};

}

class Utf16String {
public:
    [[nodiscard]] static Utf16String create();
    [[nodiscard]] static Utf16String create(Utf16Data);
    [[nodiscard]] static Utf16String create(StringView);
    [[nodiscard]] static Utf16String create(Utf16View const&);
    [[nodiscard]] static Utf16String invalid();

    Utf16Data const& string() const;
    Utf16View view() const;
    Utf16View substring_view(size_t code_unit_offset, size_t code_unit_length) const;
    Utf16View substring_view(size_t code_unit_offset) const;

    [[nodiscard]] String to_utf8() const;
    [[nodiscard]] ByteString to_byte_string() const;
    u16 code_unit_at(size_t index) const;

    size_t length_in_code_units() const;
    bool is_empty() const;
    bool is_valid() const { return m_string; }

    [[nodiscard]] u32 hash() const { return m_string->hash(); }
    [[nodiscard]] bool operator==(Utf16String const& other) const
    {
        if (m_string == other.m_string)
            return true;
        return *m_string == *other.m_string;
    }

private:
    Utf16String() = default;
    explicit Utf16String(NonnullRefPtr<Detail::Utf16StringImpl>);

    RefPtr<Detail::Utf16StringImpl> m_string;
};

}

namespace AK {

template<>
struct Traits<JS::Utf16String> : public DefaultTraits<JS::Utf16String> {
    static unsigned hash(JS::Utf16String const& s) { return s.hash(); }
};

template<>
class Optional<JS::Utf16String> : public OptionalBase<JS::Utf16String> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Utf16String;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Utf16String> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Utf16String>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Utf16String>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Utf16String>> && IsConstructible<JS::Utf16String, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    Optional& operator=(V)
    {
        clear();
        return *this;
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    void clear()
    {
        m_value = JS::Utf16String::invalid();
    }

    [[nodiscard]] bool has_value() const
    {
        return m_value.is_valid();
    }

    [[nodiscard]] JS::Utf16String& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Utf16String const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Utf16String value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Utf16String release_value()
    {
        VERIFY(has_value());
        JS::Utf16String released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Utf16String m_value { JS::Utf16String::invalid() };
};

}
