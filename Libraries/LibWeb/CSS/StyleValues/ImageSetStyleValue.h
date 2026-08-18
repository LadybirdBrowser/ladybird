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

    virtual void load_any_resources(DOM::Document&) override;

    virtual Optional<CSSPixels> natural_width(DOM::Document const&) const override;
    virtual Optional<CSSPixels> natural_height(DOM::Document const&) const override;
    virtual Optional<CSSPixelFraction> natural_aspect_ratio(DOM::Document const&) const override;

    virtual ResolvedImage resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;
    virtual bool is_paintable(DOM::Document const&) const override;
    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&, ResolvedImage const&) const override;
    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap(DOM::Document const&) const override;

    AbstractImageStyleValue const* selected_image() const { return m_selected_image; }

private:
    explicit ImageSetStyleValue(Vector<Option>);
    explicit ImageSetStyleValue(StyleValueFFI::StyleValueData const*);

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    void set_style_sheet(GC::Ptr<CSSStyleSheet>);
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    Optional<Option> select_option(double device_pixels_per_css_pixel) const;

    Vector<Option> const& options() const
    {
        if (m_options.has_value())
            return *m_options;
        auto const& list = m_value->image_set.options;
        Vector<Option> options;
        options.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i) {
            auto const& option = list.pointer[i];
            Optional<Utf16String> type;
            if (option.has_type)
                type = Utf16String::from_raw(option.type_string.raw);
            options.unchecked_append(Option {
                .image = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                                                                     static_cast<StyleValueFFI::StyleValueData const*>(option.image.pointer)))
                    ->as_abstract_image(),
                .resolution = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                    static_cast<StyleValueFFI::StyleValueData const*>(option.resolution.pointer))),
                .type = move(type),
            });
        }
        m_options = move(options);
        return *m_options;
    }

    static StyleValueFFI::StyleValueData const* make_image_set_data(Vector<Option> const&);

    mutable Optional<Vector<Option>> m_options;
    mutable AbstractImageStyleValue const* m_selected_image { nullptr };
    mutable double m_selected_resolution { 1 };
};

}
