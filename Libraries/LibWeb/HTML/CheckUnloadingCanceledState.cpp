/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CheckUnloadingCanceledState.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CheckUnloadingCanceledState);

CheckUnloadingCanceledState::CheckUnloadingCanceledState(
    GC::Ptr<LocalTraversableNavigable> owner,
    GC::Ptr<LocalTraversableNavigable> traversable,
    Optional<UserNavigationInvolvement> user_involvement,
    UnloadPromptShown unload_prompt_shown,
    GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> callback)
    : m_unload_prompt_shown(unload_prompt_shown)
    , m_owner(owner)
    , m_traversable(traversable)
    , m_user_involvement(user_involvement)
    , m_callback(callback)
    , m_timeout(Platform::Timer::create_single_shot(heap(), TIMEOUT_MS, GC::create_function(heap(), [this] {
        if (!m_completed) {
            if (has_pending_before_unload_dialog()) {
                m_timeout->restart();
                return;
            }
            dbgln("FIXME: check_if_unloading_is_canceled timed out");
            finish(Result::Continue);
        }
    })))
{
    m_timeout->start();
}

void CheckUnloadingCanceledState::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_phase2_documents);
    visitor.visit(m_documents_already_fired);
    visitor.visit(m_traversable);
    visitor.visit(m_owner);
    visitor.visit(m_callback);
    visitor.visit(m_close_preflight_callback);
    visitor.visit(m_timeout);
}

void CheckUnloadingCanceledState::adopt_for_close(GC::Ref<GC::Function<void(bool)>> callback)
{
    VERIFY(!m_close_preflight_callback);
    m_close_preflight_callback = callback;
    m_adopted_for_close = true;
    ++m_phase2_generation;
}

void CheckUnloadingCanceledState::start(Vector<GC::Root<LocalNavigable>> const& navigables_that_need_before_unload, RefPtr<SessionHistoryEntry> target_entry)
{
    // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
    for (auto& navigable : navigables_that_need_before_unload)
        m_phase2_documents.append(*navigable->active_document());

    // 2. Let unloadPromptShown be false.

    // 3. Let finalStatus be "continue".

    // 4. If traversable was given, then:
    if (m_traversable) {
        // 1. Assert: targetStep and userInvolvementForNavigateEvent were given.
        VERIFY(target_entry);
        VERIFY(m_user_involvement.has_value());

        // 2. Let targetEntry be the result of getting the target history entry given traversable and targetStep.
        m_target_entry = move(target_entry);

        // 3. If targetEntry is not traversable's current session history entry, and targetEntry's document state's origin is the same as
        //    traversable's current session history entry's document state's origin:
        if (m_target_entry != m_traversable->current_session_history_entry() && m_target_entry->document_state()->origin() == m_traversable->current_session_history_entry()->document_state()->origin()) {

            // 1. Let eventsFired be false.

            // 2. Let needsBeforeunload be true if navigablesThatNeedBeforeUnload contains traversable; otherwise false.
            m_needs_beforeunload = navigables_that_need_before_unload.find_if([this](auto const& navigable) {
                return navigable.ptr() == m_traversable.ptr();
            }) != navigables_that_need_before_unload.end();

            // 3. If needsBeforeunload is true, then remove traversable's active document from documentsToFireBeforeunload.
            if (m_needs_beforeunload) {
                m_phase2_documents.remove_first_matching([this](auto& document) {
                    return document.ptr() == m_traversable->active_document().ptr();
                });
            }

            start_phase1();
            return;
        }
    }

    start_phase2();
}

bool CheckUnloadingCanceledState::document_was_already_fired(DOM::Document const& document) const
{
    return m_documents_already_fired.find_if([&](auto const& fired_document) {
        return fired_document.ptr() == &document;
    }) != m_documents_already_fired.end();
}

void CheckUnloadingCanceledState::fire_beforeunload(DOM::Document& document, bool unload_prompt_shown, bool& prompt_shown, bool& prompt_canceled)
{
    if (!document_was_already_fired(document))
        m_documents_already_fired.append(document);

    if (m_owner)
        m_owner->set_active_beforeunload_check(Badge<CheckUnloadingCanceledState> {}, this);
    auto [did_show_prompt, did_cancel] = document.steps_to_fire_beforeunload(unload_prompt_shown);
    prompt_shown = did_show_prompt;
    prompt_canceled = did_cancel;
    if (m_owner && m_owner->active_beforeunload_check(Badge<CheckUnloadingCanceledState> {}).ptr() == this)
        m_owner->set_active_beforeunload_check(Badge<CheckUnloadingCanceledState> {}, nullptr);
}

void CheckUnloadingCanceledState::continue_as_close_after_adoption(bool prompt_shown, bool prompt_canceled)
{
    if (prompt_shown)
        m_unload_prompt_shown = UnloadPromptShown::Yes;
    if (prompt_canceled) {
        finish(Result::CanceledByBeforeUnload);
        return;
    }

    m_running_adopted_close = true;
    m_phase2_documents.clear();
    m_remaining_phase2_tasks = 0;
    if (m_owner) {
        if (auto active_document = m_owner->active_document()) {
            for (auto& navigable : active_document->inclusive_descendant_navigables()) {
                auto* local_navigable = as_if<LocalNavigable>(navigable.ptr());
                auto document = local_navigable ? local_navigable->active_document() : GC::Ptr<DOM::Document> {};
                if (document && !document_was_already_fired(*document))
                    m_phase2_documents.append(*document);
            }
        }
    }
    start_phase2();
}

bool CheckUnloadingCanceledState::has_pending_before_unload_dialog() const
{
    if (m_traversable) {
        if (auto active_document = m_traversable->active_document(); active_document && active_document->page().pending_dialog() == Page::PendingDialog::BeforeUnload)
            return true;
    }

    return m_phase2_documents.find_if([](auto const& document) {
        return document->page().pending_dialog() == Page::PendingDialog::BeforeUnload;
    }) != m_phase2_documents.end();
}

void CheckUnloadingCanceledState::start_phase1()
{
    // 4. Queue a global task on the navigation and traversal task source given traversable's active window to perform the following steps:
    VERIFY(m_traversable->active_window());
    queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*m_traversable->active_window()), GC::create_function(GC::Heap::the(), [this] {
        // 1. if needsBeforeunload is true, then:
        if (m_needs_beforeunload) {
            // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given traversable's active document and false.
            bool unload_prompt_shown_for_this_document;
            bool unload_prompt_canceled_by_this_document;
            fire_beforeunload(*m_traversable->active_document(), false, unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document);
            if (m_adopted_for_close && !m_running_adopted_close) {
                continue_as_close_after_adoption(unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document);
                return;
            }

            // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
            if (unload_prompt_shown_for_this_document)
                m_unload_prompt_shown = UnloadPromptShown::Yes;

            // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
            if (unload_prompt_canceled_by_this_document)
                m_final_status = Result::CanceledByBeforeUnload;
        }

        // 2. If finalStatus is "canceled-by-beforeunload", then abort these steps.
        if (m_final_status == Result::CanceledByBeforeUnload) {
            finish(m_final_status);
            return;
        }

        // 3. Let navigation be traversable's active window's navigation API.
        VERIFY(m_traversable->active_window());
        auto navigation = m_traversable->active_window()->navigation();

        // 4. Let navigateEventResult be the result of firing a traverse navigate event at navigation given targetEntry and userInvolvementForNavigateEvent.
        VERIFY(m_target_entry);
        auto navigate_event_result = navigation->fire_a_traverse_navigate_event(*m_target_entry, *m_user_involvement);

        // 5. If navigateEventResult is false, then set finalStatus to "canceled-by-navigate".
        if (!navigate_event_result)
            m_final_status = Result::CanceledByNavigate;

        // 6. Set eventsFired to true.

        phase1_completed();
    }));
}

void CheckUnloadingCanceledState::phase1_completed()
{
    // 5. Wait for eventsFired to be true.

    // 6. If finalStatus is not "continue", then return finalStatus.
    if (m_final_status != Result::Continue) {
        finish(m_final_status);
        return;
    }
    start_phase2();
}

void CheckUnloadingCanceledState::start_phase2()
{
    if (m_phase2_documents.is_empty()) {
        finish(m_final_status);
        return;
    }

    // 5. Let totalTasks be the size of documentsToFireBeforeunload.

    // 6. Let completedTasks be 0.
    m_remaining_phase2_tasks = m_phase2_documents.size();

    // 7. For each document of documentsToFireBeforeunload, queue a global task on the navigation and traversal task source given document's relevant global object to run the steps:
    auto generation = m_phase2_generation;
    for (auto& document : m_phase2_documents) {
        // AD-HOC: Queue with a null document instead of using queue_global_task. Tasks associated with a document
        //         are only runnable when fully active. In the async state machine, documents can become non
        //         fully-active between queue and execution time, causing the task to be permanently stuck.
        //         A null-document task is always runnable; we check validity inside.
        queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr, GC::create_function(heap(), [this, document, generation] {
            if (generation != m_phase2_generation)
                return;
            if (document->has_been_destroyed() || !document->is_fully_active()) {
                did_complete_phase2_task();
                return;
            }

            // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given document and unloadPromptShown.
            bool unload_prompt_shown_for_this_document;
            bool unload_prompt_canceled_by_this_document;
            fire_beforeunload(*document, m_unload_prompt_shown == UnloadPromptShown::Yes, unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document);
            if (m_adopted_for_close && !m_running_adopted_close) {
                continue_as_close_after_adoption(unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document);
                return;
            }
            if (generation != m_phase2_generation)
                return;

            // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
            if (unload_prompt_shown_for_this_document)
                m_unload_prompt_shown = UnloadPromptShown::Yes;

            // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
            if (unload_prompt_canceled_by_this_document)
                m_final_status = Result::CanceledByBeforeUnload;

            // 4. Increment completedTasks.
            did_complete_phase2_task();
        }));
    }
}

void CheckUnloadingCanceledState::did_complete_phase2_task()
{
    VERIFY(m_remaining_phase2_tasks > 0);
    if (--m_remaining_phase2_tasks > 0)
        return;

    // 8. Wait for completedTasks to be totalTasks.

    // 9. Return finalStatus.
    finish(m_final_status);
}

void CheckUnloadingCanceledState::finish(Result final_result)
{
    if (m_completed)
        return;
    m_completed = true;
    m_timeout->stop();
    if (m_adopted_for_close) {
        m_callback->function()(final_result == Result::CanceledByBeforeUnload ? Result::CanceledByBeforeUnload : Result::CanceledByNavigate, m_unload_prompt_shown);
        VERIFY(m_close_preflight_callback);
        m_close_preflight_callback->function()(final_result == Result::Continue);
        return;
    }
    m_callback->function()(final_result, m_unload_prompt_shown);
}

}
