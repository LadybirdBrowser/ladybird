/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/OwnPtr.h>
#include <LibGC/Function.h>
#include <LibGfx/Forward.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/ViewportClient.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LazyLoadingElement.h>
#include <LibWeb/HTML/SourceSet.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::HTML {

class HTMLImageElement final
    : public HTMLElement
    , public FormAssociatedElement
    , public LazyLoadingElement<HTMLImageElement>
    , public Layout::ImageProvider
    , public DOM::ViewportClient {
    WEB_PLATFORM_OBJECT(HTMLImageElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLImageElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLImageElement);
    LAZY_LOADING_ELEMENT(HTMLImageElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~HTMLImageElement() override;

    virtual void form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    Optional<String> alternative_text() const override
    {
        if (auto alt = get_attribute(HTML::AttributeNames::alt); alt.has_value())
            return alt.release_value();
        return {};
    }

    String alt() const { return get_attribute_value(HTML::AttributeNames::alt); }

    RefPtr<Gfx::ImmutableBitmap> immutable_bitmap() const;
    virtual RefPtr<Gfx::ImmutableBitmap> default_image_bitmap_sized(Gfx::IntSize) const override;

    WebIDL::UnsignedLong width() const;
    void set_width(WebIDL::UnsignedLong);

    WebIDL::UnsignedLong height() const;
    void set_height(WebIDL::UnsignedLong);

    unsigned natural_width() const;
    unsigned natural_height() const;

    int x() const;
    int y() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-complete
    bool complete() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-currentsrc
    String current_src() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-decode
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> decode() const;

    virtual Optional<ARIA::Role> default_role() const override;

    // https://html.spec.whatwg.org/multipage/images.html#img-environment-changes
    void react_to_changes_in_the_environment();

    // https://html.spec.whatwg.org/multipage/images.html#update-the-image-data
    void update_the_image_data(bool restart_the_animations = false, bool maybe_omit_events = false);

    // https://html.spec.whatwg.org/multipage/images.html#use-srcset-or-picture
    [[nodiscard]] bool uses_srcset_or_picture() const;

    // https://html.spec.whatwg.org/multipage/rendering.html#restart-the-animation
    void restart_the_animation();

    // https://html.spec.whatwg.org/multipage/images.html#select-an-image-source
    [[nodiscard]] Optional<ImageSourceAndPixelDensity> select_an_image_source();

    void set_source_set(SourceSet);

    // https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element:dimension-attribute-source
    DOM::Element const& dimension_attribute_source() const;
    void set_dimension_attribute_source(DOM::Element const*);

    ImageRequest& current_request() { return *m_current_request; }
    ImageRequest const& current_request() const { return *m_current_request; }

    virtual size_t current_frame_index() const override { return m_current_frame_index; }

    // https://html.spec.whatwg.org/multipage/images.html#upgrade-the-pending-request-to-the-current-request
    void upgrade_pending_request_to_current_request();

    // https://html.spec.whatwg.org/multipage/embedded-content.html#allows-auto-sizes
    bool allows_auto_sizes() const;

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap_sized(Gfx::IntSize) const override;
    virtual void set_visible_in_viewport(bool) override;
    virtual GC::Ptr<DOM::Element const> to_html_element() const override { return *this; }
    virtual GC::Ptr<DecodedImageData> decoded_image_data() const override;

    virtual void visit_edges(Cell::Visitor&) override;

private:
    HTMLImageElement(DOM::Document&, DOM::QualifiedName);

    void update_the_image_data_impl(bool restart_the_animations, bool maybe_omit_events, u64 update_the_image_data_count);

    virtual bool is_html_image_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    virtual void adopted_from(DOM::Document&) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
    virtual void adjust_computed_style(CSS::ComputedProperties&) override;

    virtual void did_set_viewport_rect(CSSPixelRect const&) override;

    void handle_successful_fetch(URL::URL const&, StringView mime_type, ImageRequest&, ByteBuffer, bool maybe_omit_events, URL::URL const& previous_url);
    void handle_failed_fetch();
    void add_callbacks_to_image_request(GC::Ref<ImageRequest>, bool maybe_omit_events, String const& url_string, String const& previous_url);

    void animate();

    RefPtr<Core::Timer> m_animation_timer;
    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };

    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    GC::Ptr<DOM::DocumentObserver> m_document_observer;

    CORSSettingAttribute m_cors_setting { CORSSettingAttribute::NoCORS };

    // https://html.spec.whatwg.org/multipage/images.html#last-selected-source
    // Each img element has a last selected source, which must initially be null.
    Optional<String> m_last_selected_source;

    // https://html.spec.whatwg.org/multipage/images.html#current-request
    GC::Ptr<ImageRequest> m_current_request;

    // https://html.spec.whatwg.org/multipage/images.html#pending-request
    GC::Ptr<ImageRequest> m_pending_request;

    SourceSet m_source_set;

    CSSPixelSize m_last_seen_viewport_size;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element:dimension-attribute-source
    // Each img element has a dimension attribute source, which must initially be the img element itself.
    GC::Ptr<DOM::Element const> m_dimension_attribute_source;

    u64 m_update_the_image_data_count { 0 };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLImageElement>() const { return is_html_image_element(); }

}
