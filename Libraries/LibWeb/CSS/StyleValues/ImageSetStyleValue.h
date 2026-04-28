/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>

namespace Web::CSS {

class ImageSetStyleValue final : public AbstractImageStyleValue {
    using Base = AbstractImageStyleValue;

public:
    struct Option {
        NonnullRefPtr<AbstractImageStyleValue const> image;
        NonnullRefPtr<StyleValue const> resolution;
        Optional<String> type;
    };

    static ValueComparingNonnullRefPtr<ImageSetStyleValue const> create(Vector<Option>);
    virtual ~ImageSetStyleValue() override = default;

    virtual void visit_edges(JS::Cell::Visitor&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual bool equals(StyleValue const& other) const override;
    virtual bool is_computationally_independent() const override;

    virtual void load_any_resources(DOM::Document&) override;
    virtual void load_any_resources(Layout::NodeWithStyle const&) override;

    virtual Optional<CSSPixels> natural_width() const override;
    virtual Optional<CSSPixels> natural_height() const override;
    virtual Optional<CSSPixelFraction> natural_aspect_ratio() const override;

    virtual void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;
    virtual bool is_paintable() const override;
    virtual void paint(DisplayListRecordingContext&, DevicePixelRect const&, ImageRendering) const override;
    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;

    AbstractImageStyleValue const* selected_image() const { return m_selected_image; }

private:
    explicit ImageSetStyleValue(Vector<Option>);

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    AbstractImageStyleValue const* select_image(double device_pixels_per_css_pixel, Optional<CalculationResolutionContext> const&) const;
    void update_selected_image_for_layout_node(Layout::NodeWithStyle const&) const;

    Vector<Option> m_options;
    GC::Ptr<CSSStyleSheet> m_style_sheet;
    mutable AbstractImageStyleValue const* m_selected_image { nullptr };
};

}
