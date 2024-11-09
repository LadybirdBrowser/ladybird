/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class TextMetrics : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TextMetrics, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(TextMetrics);

public:
    [[nodiscard]] static JS::NonnullGCPtr<TextMetrics> create(JS::Realm&);

    virtual ~TextMetrics() override;

    double width() const { return m_width; }
    double actual_bounding_box_left() const { return m_actual_bounding_box_left; }
    double actual_bounding_box_right() const { return m_actual_bounding_box_right; }
    double font_bounding_box_ascent() const { return m_font_bounding_box_ascent; }
    double font_bounding_box_descent() const { return m_font_bounding_box_descent; }
    double actual_bounding_box_ascent() const { return m_actual_bounding_box_ascent; }
    double actual_bounding_box_descent() const { return m_actual_bounding_box_descent; }
    double em_height_ascent() const { return m_em_height_ascent; }
    double em_height_descent() const { return m_em_height_descent; }
    double hanging_baseline() const { return m_hanging_baseline; }
    double alphabetic_baseline() const { return m_alphabetic_baseline; }
    double ideographic_baseline() const { return m_ideographic_baseline; }

    void set_width(double width) { m_width = width; }
    void set_actual_bounding_box_left(double left) { m_actual_bounding_box_left = left; }
    void set_actual_bounding_box_right(double right) { m_actual_bounding_box_right = right; }
    void set_font_bounding_box_ascent(double ascent) { m_font_bounding_box_ascent = ascent; }
    void set_font_bounding_box_descent(double descent) { m_font_bounding_box_descent = descent; }
    void set_actual_bounding_box_ascent(double ascent) { m_actual_bounding_box_ascent = ascent; }
    void set_actual_bounding_box_descent(double descent) { m_actual_bounding_box_descent = descent; }
    void set_em_height_ascent(double ascent) { m_em_height_ascent = ascent; }
    void set_em_height_descent(double descent) { m_em_height_descent = descent; }
    void set_hanging_baseline(double baseline) { m_hanging_baseline = baseline; }
    void set_alphabetic_baseline(double baseline) { m_alphabetic_baseline = baseline; }
    void set_ideographic_baseline(double baseline) { m_ideographic_baseline = baseline; }

private:
    explicit TextMetrics(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    double m_width { 0 };
    double m_actual_bounding_box_left { 0 };
    double m_actual_bounding_box_right { 0 };

    double m_font_bounding_box_ascent { 0 };
    double m_font_bounding_box_descent { 0 };
    double m_actual_bounding_box_ascent { 0 };
    double m_actual_bounding_box_descent { 0 };
    double m_em_height_ascent { 0 };
    double m_em_height_descent { 0 };
    double m_hanging_baseline { 0 };
    double m_alphabetic_baseline { 0 };
    double m_ideographic_baseline { 0 };
};

}
