/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LengthStyleValue.h"

#include <LibWeb/CSS/StyleComputeFFI.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<LengthStyleValue const> LengthStyleValue::create(Length const& length)
{
    // Small integral pixel lengths dominate real-world values (margins, paddings, borders,
    // font sizes), so they are interned: repeated creations are allocation-free and identical
    // values are pointer-identical.
    static constexpr i32 last_interned_px_value = 64;
    if (length.is_px()) {
        auto raw_value = length.raw_value();
        if (raw_value >= 0 && raw_value <= last_interned_px_value && raw_value == static_cast<i32>(raw_value)) {
            static auto const& instances = *[] {
                auto* instances = new (nothrow) Vector<NonnullRefPtr<LengthStyleValue const>>();
                instances->ensure_capacity(last_interned_px_value + 1);
                for (i32 px = 0; px <= last_interned_px_value; ++px)
                    instances->unchecked_append(adopt_ref(*new (nothrow) LengthStyleValue(Length::make_px(px))));
                return instances;
            }();
            return instances[static_cast<i32>(raw_value)];
        }
    }
    return adopt_ref(*new (nothrow) LengthStyleValue(length));
}

ValueComparingNonnullRefPtr<StyleValue const> LengthStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& context = computation_context.length_resolution_context;
    auto ffi_context = to_ffi_length_resolution_context(context);
    auto result = ComputedValuesFFI::rust_absolutize_length(length().raw_value(), to_underlying(length().unit()), &ffi_context);
    if (result.handled) {
        if (result.resolved_viewport_relative_length)
            context.record_viewport_relative_length_resolution();
        if (!result.changed)
            return *this;
        return LengthStyleValue::create(Length::make_px(result.px));
    }

    // Container-relative units are not handled in Rust yet.
    if (auto absolutized_length = length().absolutize(context); absolutized_length.has_value())
        return LengthStyleValue::create(absolutized_length.release_value());
    return *this;
}

bool LengthStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_length = other.as_length();
    return length() == other_length.length();
}

}
