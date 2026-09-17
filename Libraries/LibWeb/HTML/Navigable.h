/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class WEB_API Navigable : public JS::Cell {
    GC_CELL(Navigable, JS::Cell);

public:
    virtual ~Navigable() override;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-id
    CrossProcessId id() const { return m_id; }

    GC::Ptr<Navigable> parent() const { return m_parent; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container
    GC::Ptr<NavigableContainer> container() const;
    // NB: A page sets the container of the local navigable it created to populate the content navigable's next
    //     document there, before the navigable is the content navigable, and clears it if it never becomes one.
    void set_container(Badge<NavigableContainer, Page>, GC::Ptr<NavigableContainer> container) { m_container = container; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
    GC::Ptr<DOM::Document> container_document() const;

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    bool is_ancestor_of(Navigable const&) const;

    GC::Ptr<Navigable> find(CrossProcessId);

    template<typename T>
    bool fast_is() const = delete;

    virtual bool has_been_destroyed() const = 0;
    virtual void set_has_been_destroyed() = 0;

    virtual GC::Ptr<WindowProxy> active_window_proxy() = 0;
    virtual Utf16String const& target_name() const = 0;
    virtual bool is_traversable() const { return false; }
    virtual bool is_local_navigable() const { return false; }
    GC::Ref<Navigable> traversable_navigable();
    GC::Ref<Navigable> top_level_traversable();
    virtual bool is_top_level_traversable() const { return false; }
    virtual Optional<URL::URL> active_document_url() const = 0;
    virtual Optional<URL::Origin> active_document_origin() const = 0;
    virtual bool active_document_is_fully_active() const = 0;
    virtual bool active_document_is_completely_loaded() const = 0;
    virtual bool active_document_is(DOM::Document const&) const = 0;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#inclusive-descendant-navigables
    virtual Vector<GC::Root<Navigable>> active_document_inclusive_descendant_navigables() = 0;

    virtual Optional<URL::URL> active_document_top_level_creation_url() const = 0;
    virtual Optional<URL::Origin> active_document_top_level_origin() const = 0;
    virtual bool active_document_has_cross_site_ancestor() const = 0;
    virtual OpenerPolicy const& active_document_opener_policy() const = 0;
    virtual bool active_browsing_context_is_auxiliary() const = 0;
    virtual GC::Ptr<WindowProxy> active_browsing_context_opener_window_proxy() const = 0;

    virtual ReplicatedContainerState container_state() const = 0;
    bool container_is_in_document_tree() const { return container_state().is_in_document_tree; }
    SandboxingFlagSet container_iframe_sandboxing_flag_set() const { return container_state().iframe_sandboxing_flag_set; }
    SandboxingFlagSet container_document_active_sandboxing_flag_set() const { return container_state().document_active_sandboxing_flag_set; }
    Optional<Utf16FlyString> container_local_name() const { return container_state().local_name; }
    ReferrerPolicy::ReferrerPolicy container_iframe_referrer_policy() const { return container_state().iframe_referrer_policy; }

    virtual bool has_session_history_entry_and_ready_for_navigation() const = 0;
    virtual bool delays_the_load_event_of_its_container() const = 0;

    // https://html.spec.whatwg.org/multipage/interaction.html#currently-focused-area-of-a-top-level-traversable
    virtual GC::Ptr<DOM::Node> currently_focused_area() = 0;
    GC::Ptr<DOM::Node> currently_focused_area_shown_by_focused_navigable();

    WebIDL::ExceptionOr<void> navigate(NavigateParams);

    bool allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const&) const;

protected:
    explicit Navigable(GC::Ref<Page>);
    void set_id(CrossProcessId id) { m_id = id; }
    void set_parent(GC::Ptr<Navigable> parent) { m_parent = parent; }

    virtual WebIDL::ExceptionOr<void> continue_navigation_in_active_document_agent(PreparedNavigation) = 0;

    virtual void visit_edges(Cell::Visitor&) override;

private:
    CrossProcessId m_id;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-parent
    GC::Ptr<Navigable> m_parent;

    // Implied link between navigable and its container.
    GC::Ptr<NavigableContainer> m_container;

    GC::Ref<Page> m_page;
};

template<>
inline bool Navigable::fast_is<LocalNavigable>() const { return is_local_navigable(); }

}
