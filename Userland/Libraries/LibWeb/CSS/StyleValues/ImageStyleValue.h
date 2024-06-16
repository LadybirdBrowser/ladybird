/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/Handle.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/HTML/SharedImageRequest.h>

namespace Web::CSS {

class ImageStyleValue final
    : public AbstractImageStyleValue
    , public Weakable<ImageStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ImageStyleValue> create(URL::URL const& url)
    {
        return adopt_ref(*new (nothrow) ImageStyleValue(url));
    }
    virtual ~ImageStyleValue() override;

    void visit_edges(JS::Cell::Visitor& visitor) const
    {
        // FIXME: visit_edges in non-GC allocated classes is confusing pattern.
        //        Consider making StyleValue to be GC allocated instead.
        visitor.visit(m_image_request);
    }

    virtual String to_string() const override;
    virtual bool equals(StyleValue const& other) const override;

    virtual void load_any_resources(DOM::Document&) override;

    Optional<CSSPixels> natural_width() const override;
    Optional<CSSPixels> natural_height() const override;
    Optional<CSSPixelFraction> natural_aspect_ratio() const override;

    virtual bool is_paintable() const override;
    void paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering, Vector<Gfx::Path> const& clip_paths = {}) const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;

    Function<void()> on_animate;

    JS::GCPtr<HTML::DecodedImageData> image_data() const;

private:
    ImageStyleValue(URL::URL const&);

    JS::GCPtr<HTML::SharedImageRequest> m_image_request;

    void animate();
    Gfx::ImmutableBitmap const* bitmap(size_t frame_index, Gfx::IntSize = {}) const;

    URL::URL m_url;
    WeakPtr<DOM::Document> m_document;

    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };
    RefPtr<Platform::Timer> m_timer;
};

}
