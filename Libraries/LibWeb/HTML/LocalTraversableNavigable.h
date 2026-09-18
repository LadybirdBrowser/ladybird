/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#traversable-navigable
class WEB_API LocalTraversableNavigable final : public LocalNavigable {
    GC_CELL(LocalTraversableNavigable, LocalNavigable);
    GC_DECLARE_ALLOCATOR(LocalTraversableNavigable);

public:
    static GC::Ref<LocalTraversableNavigable> create_a_new_top_level_traversable(GC::Ref<Page>, GC::Ptr<BrowsingContext> opener, Optional<SessionHistoryEntryDescriptor> initial_history_entry = {}, VisibilityState system_visibility_state = VisibilityState::Hidden);
    static GC::Ref<LocalTraversableNavigable> create_a_fresh_top_level_traversable(GC::Ref<Page>, URL::URL const& initial_navigation_url, DocumentResource, SessionHistoryEntryDescriptor initial_history_entry, VisibilityState system_visibility_state);

    virtual ~LocalTraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    u64 session_history_entry_count() const { return m_session_history_entry_count; }
    void set_session_history_entry_count(u64 count) { m_session_history_entry_count = count; }

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    void run_ui_history_step_unload_cancelation_job(CrossProcessId operation_id, SessionHistoryEntryDescriptor target_entry, Vector<CrossProcessId> navigables_crossing_documents, UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult, UnloadPromptShown)>>);

    void reset_session_history_for_testing();

    enum class PromptToUnload : bool {
        No,
        Yes,
    };
    void close_top_level_traversable(PromptToUnload = PromptToUnload::Yes);
    void close_top_level_traversable_from_script(Navigable const& source);
    void remove_from_user_agent_top_level_traversable_set();
    void definitely_close_top_level_traversable(PromptToUnload = PromptToUnload::Yes);
    void run_ui_traversable_close_unload_task();
    void destroy_top_level_traversable();

    Utf16String const& window_handle() const { return m_window_handle; }
    void set_window_handle(Utf16String window_handle) { m_window_handle = move(window_handle); }

private:
    LocalTraversableNavigable(GC::Ref<Page>);

    virtual bool is_traversable() const override { return true; }

    // WebContent needs the canonical top-level entry count synchronously for is_script_closable().
    u64 m_session_history_entry_count { 1 };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-created-by-web-content
    bool m_is_created_by_web_content { false };

    // AD-HOC: A forced close may supersede a prompted close while its beforeunload check is still pending.
    bool m_close_steps_have_been_appended { false };

    Utf16String m_window_handle;
};

struct BrowsingContextAndDocument {
    GC::Ref<HTML::BrowsingContext> browsing_context;
    GC::Ref<DOM::Document> document;
};

BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page);

template<>
inline bool LocalNavigable::fast_is<LocalTraversableNavigable>() const { return is_traversable(); }

}
