/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>

namespace Web::CSS {

class ImageSetStyleValue final : public AbstractImageStyleValue {
    using Base = AbstractImageStyleValue;

public:
    struct Option {
        NonnullRefPtr<AbstractImageStyleValue const> image;
        NonnullRefPtr<StyleValue const> resolution;
        Optional<Utf16String> type;
    };

    static ValueComparingNonnullRefPtr<ImageSetStyleValue const> create(Vector<Option>);
    virtual ~ImageSetStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;
    bool equals(StyleValue const& other) const;
    virtual void load_any_resources(DOM::Document&) override;

    virtual Optional<CSSPixels> natural_width(DOM::Document const&) const override;
    virtual Optional<CSSPixels> natural_height(DOM::Document const&) const override;
    virtual Optional<CSSPixelFraction> natural_aspect_ratio(DOM::Document const&) const override;

    virtual void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;
    virtual bool is_paintable(DOM::Document const&) const override;
    virtual void paint(DisplayListRecordingContext&, DOM::Document const&, DevicePixelRect const&, ImageRendering) const override;
    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap(DOM::Document const&) const override;

    AbstractImageStyleValue const* selected_image() const { return m_selected_image; }

private:
    explicit ImageSetStyleValue(Vector<Option>);

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    void set_style_sheet(GC::Ptr<CSSStyleSheet>);
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    Optional<Option> select_option(double device_pixels_per_css_pixel) const;

    Vector<Option> options() const
    {
        auto const& list = m_value->image_set.options;
        Vector<Option> options;
        options.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i) {
            auto const& option = list.pointer[i];
            Optional<Utf16String> type;
            if (option.has_type)
                type = Utf16String::from_raw(option.type_string.raw);
            options.unchecked_append(Option {
                .image = *static_cast<AbstractImageStyleValue const*>(option.image.pointer),
                .resolution = *static_cast<StyleValue const*>(option.resolution.pointer),
                .type = move(type),
            });
        }
        return options;
    }

    static StyleValueFFI::StyleValueData* make_image_set_data(Vector<Option> const&);

    mutable AbstractImageStyleValue const* m_selected_image { nullptr };
    mutable double m_selected_resolution { 1 };
};

}
