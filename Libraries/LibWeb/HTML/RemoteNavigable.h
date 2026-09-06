/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>

namespace Web::HTML {

class WEB_API RemoteNavigable final : public Navigable {
    GC_CELL(RemoteNavigable, Navigable);
    GC_DECLARE_ALLOCATOR(RemoteNavigable);

public:
    static GC::Ref<RemoteNavigable> create(GC::Ref<Page>, CrossProcessId, GC::Ptr<Navigable> parent, ReplicatedNavigableState);
    virtual ~RemoteNavigable() override;

    ReplicatedNavigableState const& replicated_state() const { return m_replicated_state; }

    // A remote navigable is never destroyed from this process: the UI process discards the page hosting its children instead.
    virtual bool has_been_destroyed() const override { return false; }

    virtual GC::Ptr<WindowProxy> active_window_proxy() override;
    virtual Utf16String const& target_name() const override { return m_replicated_state.target_name; }

    virtual bool is_traversable() const override { return parent() == nullptr; }
    virtual bool is_top_level_traversable() const override { return parent() == nullptr; }

    virtual Optional<URL::URL> active_document_url() const override { return m_replicated_state.active_document_url; }
    virtual Optional<URL::Origin> active_document_origin() const override { return m_replicated_state.active_document_origin; }
    virtual bool active_document_is_fully_active() const override { return m_replicated_state.active_document_is_fully_active; }
    virtual bool active_document_is(DOM::Document const&) const override { return false; }
    virtual Vector<GC::Root<Navigable>> active_document_inclusive_descendant_navigables() override;
    virtual Optional<URL::URL> active_document_top_level_creation_url() const override { return m_replicated_state.top_level_creation_url; }
    virtual Optional<URL::Origin> active_document_top_level_origin() const override { return m_replicated_state.top_level_origin; }
    virtual bool active_document_has_cross_site_ancestor() const override { return m_replicated_state.has_cross_site_ancestor; }

    virtual bool has_session_history_entry_and_ready_for_navigation() const override;
    virtual bool delays_the_load_event_of_its_container() const override;

private:
    RemoteNavigable(GC::Ref<Page>, CrossProcessId, GC::Ptr<Navigable> parent, ReplicatedNavigableState);

    virtual WebIDL::ExceptionOr<void> continue_navigation_in_active_document_agent(PreparedNavigation) override;

    ReplicatedNavigableState m_replicated_state;
};

}
