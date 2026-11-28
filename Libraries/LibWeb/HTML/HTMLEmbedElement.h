/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::HTML {

class HTMLEmbedElement final
    : public NavigableContainer
    , public Layout::ImageProvider {
    WEB_WRAPPABLE(HTMLEmbedElement, NavigableContainer);
    GC_DECLARE_ALLOCATOR(HTMLEmbedElement);

public:
    virtual ~HTMLEmbedElement() override;

    CSS::SizeWithAspectRatio const& natural_size_of_content_document() const { return m_natural_size; }
    void set_natural_size_of_content_document(CSS::SizeWithAspectRatio const&);
    void rendering_changed();
    void ancestor_object_started_showing_fallback_content();

private:
    HTMLEmbedElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void initialize_element() override;
    virtual void adopted_from(DOM::Document&) override;
    virtual void post_connection() override;
    virtual void removed_from(IsSubtreeRoot, DOM::Node* old_ancestor, DOM::Node& old_root) override;
    virtual void attribute_changed(Utf16FlyString const&, Optional<Utf16String> const&, Optional<Utf16String> const&, Optional<Utf16FlyString> const&) override;
    virtual CSS::ElementBoxKind box_kind() const override;
    virtual GC::Ptr<DecodedImageData> decoded_image_data() const override;

    void queue_setup_steps();
    bool is_potentially_active();
    void run_setup_steps(u64 generation);
    void handle_response(Fetch::Infrastructure::ResponseView&, u64 generation);
    void stop_loading();
    void update_representation();

    u64 m_setup_generation { 0 };
    bool m_was_rendered { false };
    CSS::SizeWithAspectRatio m_natural_size;
    GC::Ptr<SharedResourceRequest> m_image_request;
    GC::Ptr<DOM::DocumentObserver> m_document_observer;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}
