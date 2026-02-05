/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace JS {

struct Attribute {
    enum {
        Writable = 1 << 0,
        Enumerable = 1 << 1,
        Configurable = 1 << 2,
        // AD-HOC: This is used for reporting unimplemented IDL interfaces.
        Unimplemented = 1 << 3,
    };
};

// 6.1.7.1 Property Attributes, https://tc39.es/ecma262/#sec-property-attributes
class PropertyAttributes {
public:
    constexpr PropertyAttributes(u8 bits = 0)
        : m_bits(bits)
    {
    }

    [[nodiscard]] constexpr bool is_writable() const { return m_bits & Attribute::Writable; }
    [[nodiscard]] constexpr bool is_enumerable() const { return m_bits & Attribute::Enumerable; }
    [[nodiscard]] constexpr bool is_configurable() const { return m_bits & Attribute::Configurable; }
    [[nodiscard]] constexpr bool is_unimplemented() const { return m_bits & Attribute::Unimplemented; }

    constexpr void set_writable(bool writable = true)
    {
        if (writable)
            m_bits |= Attribute::Writable;
        else
            m_bits &= ~Attribute::Writable;
    }

    constexpr void set_enumerable(bool enumerable = true)
    {
        if (enumerable)
            m_bits |= Attribute::Enumerable;
        else
            m_bits &= ~Attribute::Enumerable;
    }

    constexpr void set_configurable(bool configurable = true)
    {
        if (configurable)
            m_bits |= Attribute::Configurable;
        else
            m_bits &= ~Attribute::Configurable;
    }

    constexpr bool operator==(PropertyAttributes const& other) const { return m_bits == other.m_bits; }

    [[nodiscard]] constexpr u8 bits() const { return m_bits; }

private:
    u8 m_bits;
};

static constexpr PropertyAttributes default_attributes = Attribute::Configurable | Attribute::Writable | Attribute::Enumerable;

}

namespace AK {

template<>
struct Formatter<JS::PropertyAttributes> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::PropertyAttributes const& property_attributes)
    {
        Vector<ByteString> parts;
        parts.append(ByteString::formatted("[[Writable]]: {}", property_attributes.is_writable()));
        parts.append(ByteString::formatted("[[Enumerable]]: {}", property_attributes.is_enumerable()));
        parts.append(ByteString::formatted("[[Configurable]]: {}", property_attributes.is_configurable()));
        return Formatter<StringView>::format(builder, ByteString::formatted("PropertyAttributes {{ {} }}", ByteString::join(", "sv, parts)));
    }
};

}
