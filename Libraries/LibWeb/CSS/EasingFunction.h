/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// An easing function, held as the descriptor the Rust animation code evaluates.
class EasingFunction {
public:
    using Descriptor = Compositing::RustFFI::FfiEasingDescriptor;
    using LinearPoint = Compositing::RustFFI::FfiLinearEasingPoint;

    static EasingFunction linear();
    static EasingFunction ease();

    static EasingFunction from_style_value(StyleValue const&);

    // The descriptor borrows this function's control points.
    Descriptor descriptor() const
    {
        auto descriptor = m_descriptor;
        descriptor.linear_points = m_linear_points.data();
        descriptor.linear_point_count = m_linear_points.size();
        return descriptor;
    }

    // The descriptor reads its control points from a copy in storage the caller keeps alive.
    Descriptor descriptor_with_points_in(Vector<LinearPoint>& storage) const
    {
        storage.extend(m_linear_points);
        auto descriptor = m_descriptor;
        descriptor.linear_points = storage.data();
        descriptor.linear_point_count = storage.size();
        return descriptor;
    }

    double evaluate_at(double input_progress, bool before_flag) const;
    Utf16String const& to_utf16_string() const { return m_text; }

    // NB: The serialization is of the value the function was made from, so it tells functions apart.
    bool operator==(EasingFunction const& other) const { return m_text == other.m_text; }

private:
    Descriptor m_descriptor {};
    Vector<LinearPoint> m_linear_points;
    Utf16String m_text;
};

}
