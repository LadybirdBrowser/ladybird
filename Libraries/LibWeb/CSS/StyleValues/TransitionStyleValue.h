/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class TransitionStyleValue final : public StyleValueWithDefaultOperators<TransitionStyleValue> {
public:
    struct Transition {
        ValueComparingRefPtr<CustomIdentStyleValue> property_name;
        TimeOrCalculated duration { CSS::Time::make_seconds(0.0) };
        TimeOrCalculated delay { CSS::Time::make_seconds(0.0) };
        ValueComparingRefPtr<EasingStyleValue> easing;

        bool operator==(Transition const&) const = default;
    };

    static ValueComparingNonnullRefPtr<TransitionStyleValue> create(Vector<Transition> transitions)
    {
        return adopt_ref(*new (nothrow) TransitionStyleValue(move(transitions)));
    }

    virtual ~TransitionStyleValue() override = default;

    auto const& transitions() const { return m_transitions; }

    virtual String to_string() const override;

    bool properties_equal(TransitionStyleValue const& other) const;

private:
    explicit TransitionStyleValue(Vector<Transition> transitions)
        : StyleValueWithDefaultOperators(Type::Transition)
        , m_transitions(move(transitions))
    {
    }

    Vector<Transition> m_transitions;
};

}
