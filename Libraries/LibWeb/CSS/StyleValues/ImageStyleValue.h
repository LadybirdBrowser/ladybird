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
#include <LibURL/URL.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class ImageStyleValue;

class ImageStyleValueResource {
public:
    explicit ImageStyleValueResource(::URL::URL);
    ~ImageStyleValueResource();

    void visit_edges(JS::Cell::Visitor&);

    void set_resource_request(DOM::Document&, GC::Ref<HTML::SharedResourceRequest>);
    void register_image_style_value(DOM::Document&, ImageStyleValue const&);
    void unregister_image_style_value(ImageStyleValue const&);
    bool can_be_removed() const { return m_image_style_values.is_empty(); }

    [[nodiscard]] GC::Ptr<HTML::DecodedImageData> image_data() const;
    [[nodiscard]] Optional<Gfx::DecodedImageFrame> frame(size_t frame_index, Gfx::IntSize = {}) const;
    [[nodiscard]] size_t current_frame_index() const { return m_current_frame_index; }
    [[nodiscard]] bool has_active_animation_timer() const;

    void animate(DOM::Document&);

private:
    void add_callbacks_if_needed(DOM::Document&);
    void notify_image_style_values_did_update(DOM::Document&);
    void start_animation_timer_if_needed(DOM::Document&);
    void stop_animation_timer();
    bool is_animatable() const;
    bool animation_has_completed() const;
    int current_frame_duration() const;

    ::URL::URL m_url;
    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    GC::Ptr<Platform::Timer> m_timer;
    HashTable<ImageStyleValue const*> m_image_style_values;
    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };
    bool m_has_resource_request_callbacks { false };
};

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
        Optional<::URL::URL> m_registered_url;
    };

    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&, Optional<::URL::URL> style_resource_base_url);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(::URL::URL const&);
    virtual ~ImageStyleValue() override;
    static u64 active_animation_timer_count(DOM::Document const&);

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual bool equals(StyleValue const& other) const override;

    virtual bool is_computationally_independent() const override { return true; }

    virtual void load_any_resources(DOM::Document&) override;

    Optional<CSSPixels> natural_width(DOM::Document const&) const override;
    Optional<CSSPixels> natural_height(DOM::Document const&) const override;
    Optional<CSSPixelFraction> natural_aspect_ratio(DOM::Document const&) const override;

    virtual bool is_paintable(DOM::Document const&) const override;
    void paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap(DOM::Document const&) const override;
    Optional<Gfx::DecodedImageFrame> current_frame(DOM::Document const&, DevicePixelRect const& dest_rect) const;
    size_t current_frame_index(DOM::Document const&) const;

    mutable Function<void()> on_animate;

    GC::Ptr<HTML::DecodedImageData> image_data(DOM::Document const&) const;

private:
    friend class ImageStyleValueResource;
    friend class Client;
    friend class CSSStyleSheet;
    ImageStyleValue(URL const&, Optional<::URL::URL> style_resource_base_url = {});

    void register_client(Client&) const;
    void unregister_client(Client&) const;
    void notify_clients_did_update() const;
    void notify_did_animate() const;
    void update_style_sheet_resource_context(CSSStyleSheet const&);
    Optional<::URL::URL> resolved_url(DOM::Document const&) const;
    ::URL::URL style_resource_base_url(DOM::Document const&) const;

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    Optional<Gfx::DecodedImageFrame> frame(DOM::Document const&, size_t frame_index, Gfx::IntSize = {}) const;

    URL m_url;
    Optional<::URL::URL> m_style_resource_base_url;
    Optional<bool> m_parent_style_sheet_origin_clean;
    bool m_should_absolutize_url_for_computed_value { false };

    mutable HashTable<Client*> m_clients;
};

}
