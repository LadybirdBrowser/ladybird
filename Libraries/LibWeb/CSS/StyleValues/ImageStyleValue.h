/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <LibGC/Weak.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class ImageStyleValue final
    : public AbstractImageStyleValue
    , public Weakable<ImageStyleValue> {

    using Base = AbstractImageStyleValue;

public:
    class Client {
        friend class ImageStyleValue;

    public:
        Client(DOM::Document&, ImageStyleValue const&);
        virtual ~Client();
        virtual void image_style_value_did_update(ImageStyleValue&) = 0;

    protected:
        void image_style_value_finalize();
        GC::Ptr<DOM::Document> document() const { return m_document.ptr(); }

        ImageStyleValue const& m_image_style_value;
        GC::Weak<DOM::Document> m_document;
    };

    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(::URL::URL const&);
    virtual ~ImageStyleValue() override;
    static u64 active_animation_timer_count(DOM::Document const&);

    virtual void visit_edges(JS::Cell::Visitor& visitor) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual bool equals(StyleValue const& other) const override;

    virtual bool is_computationally_independent() const override { return true; }

    virtual void load_any_resources(DOM::Document&) override;

    Optional<CSSPixels> natural_width() const override;
    Optional<CSSPixels> natural_height() const override;
    Optional<CSSPixelFraction> natural_aspect_ratio() const override;

    virtual bool is_paintable() const override;
    void paint(DisplayListRecordingContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;
    Optional<Gfx::DecodedImageFrame> current_frame(DevicePixelRect const& dest_rect) const;

    mutable Function<void()> on_animate;

    GC::Ptr<HTML::DecodedImageData> image_data() const;

private:
    friend class Client;
    ImageStyleValue(URL const&);

    void register_client(Client&) const;
    void unregister_client(Client&) const;

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    void start_animation_timer_if_needed(DOM::Document&) const;
    void stop_animation_timer() const;
    bool is_animatable() const;
    bool animation_has_completed() const;
    int current_frame_duration() const;
    void animate();
    Optional<Gfx::DecodedImageFrame> frame(size_t frame_index, Gfx::IntSize = {}) const;

    GC::Weak<HTML::SharedResourceRequest> m_resource_request;
    GC::Weak<CSSStyleSheet> m_style_sheet;

    URL m_url;
    GC::Weak<DOM::Document> m_document;

    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };

    mutable HashTable<Client*> m_clients;
    mutable GC::Weak<Platform::Timer> m_timer;
};

}
