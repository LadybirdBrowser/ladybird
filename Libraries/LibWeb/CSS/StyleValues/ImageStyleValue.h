/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/HTML/SharedResourceRequest.h>

namespace Web::CSS {

class ImageStyleValue final
    : public AbstractImageStyleValue
    , public Weakable<ImageStyleValue> {

    using Base = AbstractImageStyleValue;

public:
    static ValueComparingNonnullRefPtr<ImageStyleValue> create(URL::URL const& url)
    {
        return adopt_ref(*new (nothrow) ImageStyleValue(url));
    }
    virtual ~ImageStyleValue() override;

    virtual void visit_edges(JS::Cell::Visitor& visitor) const override;

    virtual String to_string(SerializationMode) const override;
    virtual bool equals(CSSStyleValue const& other) const override;

    virtual void load_any_resources(DOM::Document&) override;

    Optional<CSSPixels> natural_width() const override;
    Optional<CSSPixels> natural_height() const override;
    Optional<CSSPixelFraction> natural_aspect_ratio() const override;

    virtual bool is_paintable() const override;
    void paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;
    Gfx::ImmutableBitmap const* current_frame_bitmap(DevicePixelRect const& dest_rect) const;

    Function<void()> on_animate;

    GC::Ptr<HTML::DecodedImageData> image_data() const;

private:
    ImageStyleValue(URL::URL const&);

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;

    void animate();
    Gfx::ImmutableBitmap const* bitmap(size_t frame_index, Gfx::IntSize = {}) const;

    URL::URL m_url;
    WeakPtr<DOM::Document> m_document;

    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };
    GC::Ptr<Platform::Timer> m_timer;
};

}
