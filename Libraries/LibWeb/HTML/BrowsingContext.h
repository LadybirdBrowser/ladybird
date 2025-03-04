/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/Origin.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TokenizedFeatures.h>

namespace Web::HTML {

class BrowsingContext final : public JS::Cell {
    GC_CELL(BrowsingContext, JS::Cell);
    GC_DECLARE_ALLOCATOR(BrowsingContext);

public:
    struct BrowsingContextAndDocument {
        GC::Ref<BrowsingContext> browsing_context;
        GC::Ref<DOM::Document> document;
    };

    static WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_browsing_context_and_document(GC::Ref<Page> page, GC::Ptr<DOM::Document> creator, GC::Ptr<DOM::Element> embedder, GC::Ref<BrowsingContextGroup> group);
    static WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_auxiliary_browsing_context_and_document(GC::Ref<Page> page, GC::Ref<HTML::BrowsingContext> opener);

    virtual ~BrowsingContext() override;

    GC::Ref<TraversableNavigable> top_level_traversable() const;

    bool is_ancestor_of(BrowsingContext const&) const;
    bool is_familiar_with(BrowsingContext const&) const;

    bool is_top_level() const;
    bool is_auxiliary() const { return m_is_auxiliary; }

    DOM::Document const* active_document() const;
    DOM::Document* active_document();

    HTML::WindowProxy* window_proxy();
    HTML::WindowProxy const* window_proxy() const;

    void set_window_proxy(GC::Ptr<WindowProxy>);

    HTML::Window* active_window();
    HTML::Window const* active_window() const;

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    u64 virtual_browsing_context_group_id() const { return m_virtual_browsing_context_group_id; }

    GC::Ptr<BrowsingContext> top_level_browsing_context() const;

    BrowsingContextGroup* group();
    BrowsingContextGroup const* group() const;
    void set_group(BrowsingContextGroup*);

    // https://html.spec.whatwg.org/multipage/browsers.html#bcg-remove
    void remove();

    // https://html.spec.whatwg.org/multipage/origin.html#one-permitted-sandboxed-navigator
    BrowsingContext const* the_one_permitted_sandboxed_navigator() const;
    void set_the_one_permitted_sandboxed_navigator(BrowsingContext const*)
    {
        // FIXME: Implement this
    }

    bool has_navigable_been_destroyed() const;

    GC::Ptr<BrowsingContext> opener_browsing_context() const { return m_opener_browsing_context; }
    void set_opener_browsing_context(GC::Ptr<BrowsingContext> browsing_context) { m_opener_browsing_context = browsing_context; }

    void set_is_popup(TokenizedFeature::Popup is_popup) { m_is_popup = is_popup; }
    [[nodiscard]] TokenizedFeature::Popup is_popup() const { return m_is_popup; }

    SandboxingFlagSet popup_sandboxing_flag_set() const { return m_popup_sandboxing_flag_set; }
    void set_popup_sandboxing_flag_set(SandboxingFlagSet value) { m_popup_sandboxing_flag_set = value; }

private:
    explicit BrowsingContext(GC::Ref<Page>);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Page> m_page;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context
    GC::Ptr<WindowProxy> m_window_proxy;

    // https://html.spec.whatwg.org/multipage/browsers.html#opener-browsing-context
    GC::Ptr<BrowsingContext> m_opener_browsing_context;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#opener-origin-at-creation
    Optional<URL::Origin> m_opener_origin_at_creation;

    // https://html.spec.whatwg.org/multipage/browsers.html#is-popup
    TokenizedFeature::Popup m_is_popup { TokenizedFeature::Popup::No };

    // https://html.spec.whatwg.org/multipage/browsers.html#popup-sandboxing-flag-set
    SandboxingFlagSet m_popup_sandboxing_flag_set {};

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-auxiliary
    bool m_is_auxiliary { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context-initial-url
    Optional<URL::URL> m_initial_url;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#virtual-browsing-context-group-id
    u64 m_virtual_browsing_context_group_id = { 0 };

    // https://html.spec.whatwg.org/multipage/browsers.html#tlbc-group
    GC::Ptr<BrowsingContextGroup> m_group;

    GC::Ptr<BrowsingContext> m_first_child;
    GC::Ptr<BrowsingContext> m_last_child;
    GC::Ptr<BrowsingContext> m_next_sibling;
    GC::Ptr<BrowsingContext> m_previous_sibling;
};

URL::Origin determine_the_origin(Optional<URL::URL const&>, SandboxingFlagSet, Optional<URL::Origin> source_origin);

SandboxingFlagSet determine_the_creation_sandboxing_flags(BrowsingContext const&, GC::Ptr<DOM::Element> embedder);

// FIXME: Find a better home for these
bool url_matches_about_blank(URL::URL const& url);
bool url_matches_about_srcdoc(URL::URL const& url);

}
