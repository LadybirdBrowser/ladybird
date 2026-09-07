/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>

namespace Web::HTML {

class WEB_API RemoteNavigable final : public Navigable {
    GC_CELL(RemoteNavigable, Navigable);
    GC_DECLARE_ALLOCATOR(RemoteNavigable);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<RemoteNavigable> create(GC::Ref<Page>, CrossProcessId, GC::Ptr<Navigable> parent, ReplicatedNavigableState);
    virtual ~RemoteNavigable() override;

    ReplicatedNavigableState const& replicated_state() const { return m_replicated_state; }
    void set_replicated_state(ReplicatedNavigableState);

    Vector<GC::Ref<Navigable>> const& children() const { return m_children; }
    void append_child(GC::Ref<Navigable>);
    void remove_child(Navigable&);
    void replace_child(Navigable& child, GC::Ref<Navigable> replacement);
    void remove_from_all_remote_navigables();

    // The local navigable populating this navigable's next document in this page, standing beside this node until
    // the document activates and the local navigable takes the node's place in the graph.
    GC::Ptr<LocalNavigable> provisional_navigable() const { return m_provisional_navigable; }
    void set_provisional_navigable(GC::Ptr<LocalNavigable> navigable) { m_provisional_navigable = navigable; }

    virtual bool has_been_destroyed() const override { return m_has_been_destroyed; }
    virtual void set_has_been_destroyed() override { m_has_been_destroyed = true; }

    Optional<Compositor::CompositorContextId> compositor_context_id() const { return m_replicated_state.compositor_context_id; }

    // The WindowProxy standing for the navigable, which the page keeps across changes of the hosting process.
    GC::Ptr<WindowProxy> window_proxy() const { return m_window_proxy; }
    void set_window_proxy(GC::Ref<WindowProxy> window_proxy) { m_window_proxy = window_proxy; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-closing
    bool is_closing() const { return m_replicated_state.is_closing; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#document-tree-child-navigable
    Vector<GC::Root<Navigable>> document_tree_child_navigables();

    virtual GC::Ptr<WindowProxy> active_window_proxy() override;
    GC::Ref<RemoteWindow> active_window();
    virtual Utf16String const& target_name() const override { return m_replicated_state.target_name; }

    virtual bool is_traversable() const override { return parent() == nullptr; }
    virtual bool is_top_level_traversable() const override { return parent() == nullptr; }

    virtual Optional<URL::URL> active_document_url() const override { return m_replicated_state.active_document_url; }
    virtual Optional<URL::Origin> active_document_origin() const override { return m_replicated_state.active_document_origin; }
    virtual bool active_document_is_fully_active() const override { return m_replicated_state.active_document_is_fully_active; }
    virtual bool active_document_is_completely_loaded() const override { return m_replicated_state.active_document_is_completely_loaded; }
    virtual bool active_document_is(DOM::Document const&) const override { return false; }
    virtual Vector<GC::Root<Navigable>> active_document_inclusive_descendant_navigables() override;
    virtual Optional<URL::URL> active_document_top_level_creation_url() const override { return m_replicated_state.top_level_creation_url; }
    virtual Optional<URL::Origin> active_document_top_level_origin() const override { return m_replicated_state.top_level_origin; }
    virtual bool active_document_has_cross_site_ancestor() const override { return m_replicated_state.has_cross_site_ancestor; }
    virtual OpenerPolicy const& active_document_opener_policy() const override { return m_replicated_state.opener_policy; }
    virtual ReplicatedContainerState container_state() const override;

    virtual bool has_session_history_entry_and_ready_for_navigation() const override;
    virtual bool delays_the_load_event_of_its_container() const override;

private:
    RemoteNavigable(GC::Ref<Page>, CrossProcessId, GC::Ptr<Navigable> parent, ReplicatedNavigableState);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    virtual WebIDL::ExceptionOr<void> continue_navigation_in_active_document_agent(PreparedNavigation) override;

    ReplicatedNavigableState m_replicated_state;

    Vector<GC::Ref<Navigable>> m_children;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-wp
    GC::Ptr<WindowProxy> m_window_proxy;
    GC::Ptr<RemoteWindow> m_active_window;

    GC::Ptr<LocalNavigable> m_provisional_navigable;

    bool m_has_been_destroyed { false };
};

WEB_API HashTable<GC::RawRef<RemoteNavigable>>& all_remote_navigables();
WEB_API GC::Ptr<RemoteNavigable> remote_navigable_with_id(Page const&, CrossProcessId);

}
