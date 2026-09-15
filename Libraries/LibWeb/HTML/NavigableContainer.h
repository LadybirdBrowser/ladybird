/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/InitialInsertion.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>

namespace Web::HTML {

class WEB_API NavigableContainer : public HTMLElement {
    WEB_NON_IDL_WRAPPABLE(NavigableContainer, HTMLElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~NavigableContainer() override;

    static HashTable<NavigableContainer*>& all_instances();

    GC::Ptr<Navigable> content_navigable() { return m_content_navigable; }
    GC::Ptr<Navigable const> content_navigable() const { return m_content_navigable; }

    DOM::Document const* content_document() const;
    DOM::Document const* content_document_without_origin_check() const;

    HTML::WindowProxy* content_window();

    DOM::Document const* get_svg_document() const;

    void destroy_the_child_navigable();
    static void continue_destroying_the_child_navigable(Navigable&);

    void swap_content_navigable_to_remote(Badge<Page>, ReplicatedNavigableState);
    void swap_content_navigable_to_local(Badge<Page>, LocalNavigable&);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#completely-finish-loading
    void content_navigable_completely_finished_loading();

    // All elements that extend NavigableContainer "potentially delay the load event".
    // (embed, frame, iframe, and object)
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#potentially-delays-the-load-event
    bool currently_delays_the_load_event() const;

    bool content_navigable_has_session_history_entry_and_ready_for_navigation() const;

    ReplicatedContainerState replicated_container_state();

protected:
    NavigableContainer(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#shared-attribute-processing-steps-for-iframe-and-frame-elements
    Optional<URL::URL> shared_attribute_processing_steps_for_iframe_and_frame(InitialInsertion initial_insertion);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#navigate-an-iframe-or-frame
    void navigate_an_iframe_or_frame(URL::URL url, ReferrerPolicy::ReferrerPolicy referrer_policy, Optional<Utf16String> srcdoc_string = {}, InitialInsertion = InitialInsertion::No);

    void create_new_child_navigable();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#content-navigable
    GC::Ptr<Navigable> m_content_navigable { nullptr };

    void set_potentially_delays_the_load_event(bool value);

private:
    virtual bool is_navigable_container() const override { return true; }

    virtual void finalize() override;

    static void finish_destroying_the_child_navigable(Navigable&);

    bool m_potentially_delays_the_load_event { true };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::NavigableContainer>() const { return is_navigable_container(); }

}
