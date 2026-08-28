/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

class CheckUnloadingCanceledState : public GC::Cell {
    GC_CELL(CheckUnloadingCanceledState, GC::Cell);
    GC_DECLARE_ALLOCATOR(CheckUnloadingCanceledState);

public:
    using Result = CheckIfUnloadingIsCanceledResult;
    static constexpr int TIMEOUT_MS = 15000;

    CheckUnloadingCanceledState(
        GC::Ptr<LocalTraversableNavigable> owner,
        GC::Ptr<LocalTraversableNavigable> traversable,
        Optional<UserNavigationInvolvement> user_involvement,
        UnloadPromptShown unload_prompt_shown,
        GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> callback);

    virtual void visit_edges(Visitor&) override;

    void adopt_for_close(GC::Ref<GC::Function<void(bool)>> callback);
    void start(Vector<GC::Root<LocalNavigable>> const& navigables_that_need_before_unload, RefPtr<SessionHistoryEntry> target_entry);

private:
    bool document_was_already_fired(DOM::Document const&) const;
    void fire_beforeunload(DOM::Document&, bool unload_prompt_shown, bool& prompt_shown, bool& prompt_canceled);
    void continue_as_close_after_adoption(bool prompt_shown, bool prompt_canceled);
    bool has_pending_before_unload_dialog() const;
    void start_phase1();
    void phase1_completed();
    void start_phase2();
    void did_complete_phase2_task();
    void finish(Result);

    Result m_final_status { Result::Continue };
    UnloadPromptShown m_unload_prompt_shown { UnloadPromptShown::No };
    bool m_completed { false };
    GC::Ptr<LocalTraversableNavigable> m_owner;
    bool m_needs_beforeunload { false };
    size_t m_remaining_phase2_tasks { 0 };
    Vector<GC::Ref<DOM::Document>> m_phase2_documents;
    Vector<GC::Ref<DOM::Document>> m_documents_already_fired;
    GC::Ptr<LocalTraversableNavigable> m_traversable;
    RefPtr<SessionHistoryEntry> m_target_entry;
    Optional<UserNavigationInvolvement> m_user_involvement;
    GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> m_callback;
    GC::Ptr<GC::Function<void(bool)>> m_close_preflight_callback;
    GC::Ref<Platform::Timer> m_timeout;
    u64 m_phase2_generation { 0 };
    bool m_adopted_for_close { false };
    bool m_running_adopted_close { false };
};

}
