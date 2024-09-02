/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/HTML/BrowsingContext.h>
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
    , public DOM::Document::ViewportClient {
    WEB_PLATFORM_OBJECT(HTMLImageElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLImageElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLImageElement);
    LAZY_LOADING_ELEMENT(HTMLImageElement);

public:
    virtual ~HTMLImageElement() override;

    virtual void form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value) override;

    String alt() const { return get_attribute_value(HTML::AttributeNames::alt); }
    String src() const { return get_attribute_value(HTML::AttributeNames::src); }

    RefPtr<Gfx::ImmutableBitmap> immutable_bitmap() const;
    RefPtr<Gfx::Bitmap const> bitmap() const;

    unsigned width() const;
    WebIDL::ExceptionOr<void> set_width(unsigned);

    unsigned height() const;
    WebIDL::ExceptionOr<void> set_height(unsigned);

    unsigned natural_width() const;
    unsigned natural_height() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-complete
    bool complete() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-currentsrc
    String current_src() const;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-decode
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> decode() const;

    virtual Optional<ARIA::Role> default_role() const override;

    // https://html.spec.whatwg.org/multipage/images.html#img-environment-changes
    void react_to_changes_in_the_environment();

    // https://html.spec.whatwg.org/multipage/images.html#update-the-image-data
    ErrorOr<void> update_the_image_data(bool restart_the_animations = false, bool maybe_omit_events = false);

    // https://html.spec.whatwg.org/multipage/images.html#use-srcset-or-picture
    [[nodiscard]] bool uses_srcset_or_picture() const;

    // https://html.spec.whatwg.org/multipage/rendering.html#restart-the-animation
    void restart_the_animation();

    // https://html.spec.whatwg.org/multipage/images.html#select-an-image-source
    [[nodiscard]] Optional<ImageSourceAndPixelDensity> select_an_image_source();

    StringView decoding() const;

    void set_decoding(String);

    void set_source_set(SourceSet);

    ImageRequest& current_request() { return *m_current_request; }
    ImageRequest const& current_request() const { return *m_current_request; }

    size_t current_frame_index() const { return m_current_frame_index; }

    // https://html.spec.whatwg.org/multipage/images.html#upgrade-the-pending-request-to-the-current-request
    void upgrade_pending_request_to_current_request();

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const override;
    virtual void set_visible_in_viewport(bool) override;
    virtual JS::NonnullGCPtr<DOM::Element const> to_html_element() const override { return *this; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    HTMLImageElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_image_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    virtual void adopted_from(DOM::Document&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    // https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

    virtual void did_set_viewport_rect(CSSPixelRect const&) override;

    void handle_successful_fetch(URL::URL const&, StringView mime_type, ImageRequest&, ByteBuffer, bool maybe_omit_events, URL::URL const& previous_url);
    void handle_failed_fetch();
    void add_callbacks_to_image_request(JS::NonnullGCPtr<ImageRequest>, bool maybe_omit_events, URL::URL const& url_string, URL::URL const& previous_url);

    void animate();

    RefPtr<Core::Timer> m_animation_timer;
    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };

    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    CORSSettingAttribute m_cors_setting { CORSSettingAttribute::NoCORS };

    // https://html.spec.whatwg.org/multipage/images.html#last-selected-source
    // Each img element has a last selected source, which must initially be null.
    Optional<String> m_last_selected_source;

    // https://html.spec.whatwg.org/multipage/images.html#current-request
    JS::GCPtr<ImageRequest> m_current_request;

    // https://html.spec.whatwg.org/multipage/images.html#pending-request
    JS::GCPtr<ImageRequest> m_pending_request;

    SourceSet m_source_set;

    CSSPixelSize m_last_seen_viewport_size;

    // https://html.spec.whatwg.org/multipage/images.html#image-decoding-hint
    enum class ImageDecodingHint {
        Auto,
        Sync,
        Async
    };

    ImageDecodingHint m_decoding_hint = ImageDecodingHint::Auto;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLImageElement>() const { return is_html_image_element(); }
}
