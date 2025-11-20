/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractImageStyleValue.h"
#include <LibWeb/CSS/CSSImageValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
GC::Ref<CSSStyleValue> AbstractImageStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
    return CSSImageValue::create(realm, *this);
}

ColorStopListElement ColorStopListElement::absolutized(ComputationContext const& context) const
{
    auto absolutize_if_nonnull = [&context](RefPtr<StyleValue const> const& input) -> RefPtr<StyleValue const> {
        if (!input)
            return {};
        return input->absolutized(context);
    };

    return {
        .transition_hint = transition_hint,
        .color_stop = {
            .color = absolutize_if_nonnull(color_stop.color),
            .position = absolutize_if_nonnull(color_stop.position),
            .second_position = absolutize_if_nonnull(color_stop.second_position),
        },
    };
}

void serialize_color_stop_list(StringBuilder& builder, Vector<ColorStopListElement> const& color_stop_list, SerializationMode mode)
{
    bool first = true;
    for (auto const& element : color_stop_list) {
        if (!first)
            builder.append(", "sv);

        if (element.transition_hint.has_value())
            builder.appendff("{}, "sv, element.transition_hint->value->to_string(mode));

        builder.append(element.color_stop.color->to_string(mode));
        if (element.color_stop.position)
            builder.appendff(" {}"sv, element.color_stop.position->to_string(mode));
        if (element.color_stop.second_position)
            builder.appendff(" {}"sv, element.color_stop.second_position->to_string(mode));
        first = false;
    }
}

}
