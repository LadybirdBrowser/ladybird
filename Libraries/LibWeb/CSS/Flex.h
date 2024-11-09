/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-grid-2/#typedef-flex
class Flex {
public:
    enum class Type {
        Fr,
    };

    static Optional<Type> unit_from_name(StringView);

    Flex(double value, Type type);
    static Flex make_fr(double);
    Flex percentage_of(Percentage const&) const;

    String to_string() const;
    double to_fr() const;

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    bool operator==(Flex const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
    }

    int operator<=>(Flex const& other) const
    {
        auto this_fr = to_fr();
        auto other_fr = other.to_fr();

        if (this_fr < other_fr)
            return -1;
        if (this_fr > other_fr)
            return 1;
        return 0;
    }

private:
    Type m_type;
    double m_value { 0 };
};

}

template<>
struct AK::Formatter<Web::CSS::Flex> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Flex const& flex)
    {
        return Formatter<StringView>::format(builder, flex.to_string());
    }
};
