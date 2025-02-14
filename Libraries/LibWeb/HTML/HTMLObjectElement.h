/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::HTML {

class HTMLObjectElement final
    : public NavigableContainer
    , public FormAssociatedElement
    , public Layout::ImageProvider {
    WEB_PLATFORM_OBJECT(HTMLObjectElement, NavigableContainer)
    GC_DECLARE_ALLOCATOR(HTMLObjectElement);
    FORM_ASSOCIATED_ELEMENT(NavigableContainer, HTMLObjectElement)

    enum class Representation {
        Unknown,
        Image,
        ContentNavigable,
        Children,
    };

public:
    virtual ~HTMLObjectElement() override;

    virtual void form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual void form_associated_element_was_removed(DOM::Node*) override;

    String data() const;
    void set_data(String const& data) { MUST(set_attribute(HTML::AttributeNames::data, data)); }

    String type() const { return get_attribute_value(HTML::AttributeNames::type); }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-validity
    GC::Ref<ValidityState const> validity() const;

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    HTMLObjectElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_object_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
    virtual void adjust_computed_style(CSS::ComputedProperties&) override;

    bool has_ancestor_media_element_or_object_element_not_showing_fallback_content() const;

    void queue_element_task_to_run_object_representation_steps();
    void run_object_representation_handler_steps(Fetch::Infrastructure::Response const&, MimeSniff::MimeType const&, ReadonlyBytes);
    void run_object_representation_completed_steps(Representation);
    void run_object_representation_fallback_steps();

    void load_image();
    void update_layout_and_child_objects(Representation);

    void resource_did_load(Fetch::Infrastructure::Response const&, ReadonlyBytes);
    void resource_did_fail();

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const override;
    virtual void set_visible_in_viewport(bool) override;
    virtual GC::Ref<DOM::Element const> to_html_element() const override { return *this; }

    GC::Ptr<DecodedImageData> image_data() const;

    Representation m_representation { Representation::Unknown };

    GC::Ptr<SharedResourceRequest> m_resource_request;

    GC::Ptr<DOM::DocumentObserver> m_document_observer;

    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer_for_object_representation_task;
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer_for_resource_load;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLObjectElement>() const { return is_html_object_element(); }
}
