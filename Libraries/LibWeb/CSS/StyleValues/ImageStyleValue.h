/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Enums.h>
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
    public:
        Client(ImageStyleValue&);
        virtual ~Client();
        virtual void image_style_value_did_update(ImageStyleValue&) = 0;

    protected:
        void image_style_value_finalize();

        ImageStyleValue& m_image_style_value;
    };

    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(::URL::URL const&);
    virtual ~ImageStyleValue() override;

    virtual void visit_edges(JS::Cell::Visitor& visitor) const override;

    virtual String to_string(SerializationMode) const override;
    virtual bool equals(StyleValue const& other) const override;

    virtual void load_any_resources(DOM::Document&) override;

    Optional<CSSPixels> natural_width() const override;
    Optional<CSSPixels> natural_height() const override;
    Optional<CSSPixelFraction> natural_aspect_ratio() const override;

    virtual bool is_paintable() const override;
    void paint(DisplayListRecordingContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;
    Gfx::ImmutableBitmap const* current_frame_bitmap(DevicePixelRect const& dest_rect) const;

    mutable Function<void()> on_animate;

    GC::Ptr<HTML::DecodedImageData> image_data() const;

private:
    friend class Client;

    ImageStyleValue(URL const&);

    void register_client(Client&);
    void unregister_client(Client&);

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    void animate();
    Gfx::ImmutableBitmap const* bitmap(size_t frame_index, Gfx::IntSize = {}) const;

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    GC::Ptr<CSSStyleSheet> m_style_sheet;

    URL m_url;
    WeakPtr<DOM::Document> m_document;

    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };
    GC::Ptr<Platform::Timer> m_timer;

    HashTable<Client*> m_clients;
};

}
