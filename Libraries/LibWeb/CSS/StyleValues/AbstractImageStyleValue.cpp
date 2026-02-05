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
        .transition_hint = absolutize_if_nonnull(transition_hint),
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

        if (element.transition_hint) {
            element.transition_hint->serialize(builder, mode);
            builder.append(", "sv);
        }

        element.color_stop.color->serialize(builder, mode);
        if (element.color_stop.position) {
            builder.append(' ');
            element.color_stop.position->serialize(builder, mode);
        }
        if (element.color_stop.second_position) {
            builder.append(' ');
            element.color_stop.second_position->serialize(builder, mode);
        }
        first = false;
    }
}

}
