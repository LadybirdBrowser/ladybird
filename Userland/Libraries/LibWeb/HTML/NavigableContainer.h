/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Navigable.h>

namespace Web::HTML {

class NavigableContainer : public HTMLElement {
    WEB_PLATFORM_OBJECT(NavigableContainer, HTMLElement);

public:
    static JS::GCPtr<NavigableContainer> navigable_container_with_content_navigable(JS::NonnullGCPtr<Navigable> navigable);

    virtual ~NavigableContainer() override;

    static HashTable<NavigableContainer*>& all_instances();

    JS::GCPtr<Navigable> content_navigable() { return m_content_navigable; }
    JS::GCPtr<Navigable const> content_navigable() const { return m_content_navigable.ptr(); }

    BrowsingContext* nested_browsing_context()
    {
        if (m_content_navigable)
            return m_content_navigable->active_browsing_context();
        return nullptr;
    }
    BrowsingContext const* nested_browsing_context() const
    {
        if (m_content_navigable)
            return m_content_navigable->active_browsing_context();
        return nullptr;
    }

    const DOM::Document* content_document() const;
    DOM::Document const* content_document_without_origin_check() const;

    HTML::WindowProxy* content_window();

    DOM::Document const* get_svg_document() const;

    void destroy_the_child_navigable();

    // All elements that extend NavigableContainer "potentially delay the load event".
    // (embed, frame, iframe, and object)
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#potentially-delays-the-load-event
    bool currently_delays_the_load_event() const;

    bool content_navigable_initialized() const { return m_content_navigable_initialized; }

protected:
    NavigableContainer(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#shared-attribute-processing-steps-for-iframe-and-frame-elements
    Optional<URL::URL> shared_attribute_processing_steps_for_iframe_and_frame(bool initial_insertion);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#navigate-an-iframe-or-frame
    void navigate_an_iframe_or_frame(URL::URL url, ReferrerPolicy::ReferrerPolicy referrer_policy, Optional<String> srcdoc_string = {});

    WebIDL::ExceptionOr<void> create_new_child_navigable(JS::GCPtr<JS::HeapFunction<void()>> after_session_history_update = {});

    // https://html.spec.whatwg.org/multipage/document-sequences.html#content-navigable
    JS::GCPtr<Navigable> m_content_navigable { nullptr };

    void set_potentially_delays_the_load_event(bool value) { m_potentially_delays_the_load_event = value; }

    void set_content_navigable_initialized() { m_content_navigable_initialized = true; }

private:
    virtual bool is_navigable_container() const override { return true; }
    bool m_potentially_delays_the_load_event { true };
    bool m_content_navigable_initialized { false };
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::NavigableContainer>() const { return is_navigable_container(); }
}
