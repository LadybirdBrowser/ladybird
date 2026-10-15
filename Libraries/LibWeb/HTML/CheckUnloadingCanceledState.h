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
#include <LibGC/RootVector.h>
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
        GC::Ptr<LocalTraversableNavigable> traversable,
        Optional<UserNavigationInvolvement> user_involvement,
        UnloadPromptShown unload_prompt_shown,
        GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> callback);

    virtual void visit_edges(Visitor&) override;

    void start(Vector<GC::Root<LocalNavigable>> const& navigables_that_need_before_unload, RefPtr<SessionHistoryEntry> target_entry);

private:
    bool has_pending_before_unload_dialog() const;
    void start_phase1();
    void phase1_completed();
    void start_phase2();
    void did_complete_phase2_task();
    void finish(Result);

    Result m_final_status { Result::Continue };
    UnloadPromptShown m_unload_prompt_shown { UnloadPromptShown::No };
    bool m_completed { false };
    bool m_needs_beforeunload { false };
    size_t m_remaining_phase2_tasks { 0 };
    Vector<GC::Ref<DOM::Document>> m_phase2_documents;
    GC::Ptr<LocalTraversableNavigable> m_traversable;
    RefPtr<SessionHistoryEntry> m_target_entry;
    Optional<UserNavigationInvolvement> m_user_involvement;
    GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> m_callback;
    GC::Ref<Platform::Timer> m_timeout;
};

}
