/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::Animations {

struct TimeValue {
    enum class Type : u8 {
        Milliseconds,
        // FIXME: Support percentages
    };
    Type type;
    double value;

    CSS::CSSNumberish as_css_numberish() const
    {
        switch (type) {
        case Type::Milliseconds:
            return value;
        }

        VERIFY_NOT_REACHED();
    }
};

// FIXME: This struct is required since our IDL generator requires us to return nullable union types as
//        Variant<Empty, Ts...> rather than Optional<Variant<Ts...>> (although setters are forced to be
//        Optional<Variant<Ts...>>)
struct NullableCSSNumberish : FlattenVariant<Variant<Empty>, CSS::CSSNumberish> {
    using Variant::Variant;

    static NullableCSSNumberish from_optional_css_numberish_time(Optional<TimeValue> const& value)
    {
        if (value.has_value())
            return value->as_css_numberish();

        return {};
    }
};

}
