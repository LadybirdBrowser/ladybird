/*
 * Copyright (c) 2023, Emil Militzer <emil.militzer@posteo.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Display.h>

namespace Web::CSS {

class DisplayStyleValue : public StyleValueWithDefaultOperators<DisplayStyleValue> {
public:
    static ValueComparingNonnullRefPtr<DisplayStyleValue> create(Display const&);
    virtual ~DisplayStyleValue() override = default;

    virtual String to_string() const override { return m_display.to_string(); }

    Display display() const { return m_display; }

    bool properties_equal(DisplayStyleValue const& other) const { return m_display == other.m_display; }

private:
    explicit DisplayStyleValue(Display const& display)
        : StyleValueWithDefaultOperators(Type::Display)
        , m_display(display)
    {
    }

    Display m_display;
};

}
