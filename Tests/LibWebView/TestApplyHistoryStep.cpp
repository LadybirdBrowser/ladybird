/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>

static Web::HTML::CrossProcessId root_id() { return { 1, 1 }; }
static Web::HTML::CrossProcessId child_id() { return { 1, 2 }; }

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url)
{
    auto parsed_url = parse_url(url);
    return {
        .step = step,
        .url = parsed_url,
        .document_state = {
            .id = { 3, 1000 + static_cast<u64>(step) },
            .history_policy_container = Web::HTML::DocumentState::Client::Tag,
            .request_referrer = Web::Fetch::Infrastructure::Request::Referrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY,
            .initiator_origin = {},
            .origin = parsed_url.origin(),
            .about_base_url = {},
            .resource = {},
            .reload_pending = false,
            .ever_populated = true,
            .navigable_target_name = {},
            .nested_histories = {},
        },
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = Utf16String::from_utf8(url),
        .navigation_api_id = Utf16String::from_utf8(url),
        .scroll_restoration_mode = Web::HTML::ScrollRestorationMode::Auto,
        .scroll_position_data = {},
    };
}

static Web::HTML::PendingSessionHistoryEntryDescriptor pending_entry(Web::HTML::SessionHistoryEntryDescriptor entry)
{
    return {
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
    };
}

namespace {

class FakeJobRunner {
public:
    using ChangingNavigableHistoryStepJob = WebView::ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob;
    using ApplyChangingNavigableHistoryStepContinuation = WebView::ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation;

    struct UnloadCancelationJob {
        Web::HTML::SessionHistoryEntryDescriptor target_entry;
        Vector<Web::HTML::CrossProcessId> navigables_crossing_documents;
        Function<void(Web::HTML::HistoryStepResult)> on_complete;
    };
    struct ChangingJob {
        ChangingNavigableHistoryStepJob job;
        Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete;
    };
    struct Continuation {
        ApplyChangingNavigableHistoryStepContinuation continuation;
        Function<void()> on_complete;
    };
    struct NonchangingUpdate {
        Web::HTML::CrossProcessId navigable_id;
        Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index;
        Function<void()> on_complete;
    };

    WebView::ApplyHistoryStepJobs jobs()
    {
        return {
            .run_unload_cancelation_job = [this](WebView::ApplyHistoryStepJobs::UnloadCancelationJob job, Function<void(Web::HTML::HistoryStepResult)> on_complete) { unload_cancelation_jobs.append({ move(job.target_entry), move(job.navigables_crossing_documents), move(on_complete) }); },
            .select_changing_navigable_history_step_job_endpoint = [this](ChangingNavigableHistoryStepJob const& job) {
                selected_changing_job_endpoints.append(job.navigable_id);
                return true; },
            .run_changing_navigable_history_step_job = [this](ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) { changing_jobs.append({ move(job), move(on_complete) }); },
            .apply_changing_navigable_history_step_continuation = [this](ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) { continuations.append({ move(continuation), move(on_complete) }); },
            .update_nonchanging_navigable_history_step_state = [this](Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete) { nonchanging_updates.append({ navigable_id, history_object_length_and_index, move(on_complete) }); },
        };
    }

    Vector<UnloadCancelationJob> unload_cancelation_jobs;
    Vector<Web::HTML::CrossProcessId> selected_changing_job_endpoints;
    Vector<ChangingJob> changing_jobs;
    Vector<Continuation> continuations;
    Vector<NonchangingUpdate> nonchanging_updates;
};

struct TestTraversable {
    TestTraversable()
    {
        traversable.set_id(root_id());
    }

    WebView::CanonicalNavigable& add_child(Web::HTML::CrossProcessId id)
    {
        return traversable.append_child(make<WebView::CanonicalNavigable>(id, traversable.id(), nullptr, 0));
    }

    // Two top-level entries; the current entry is the second.
    void with_two_top_level_entries()
    {
        VERIFY(history.initialize_for_testing({ entry(0, "https://a.example/"sv), entry(1, "https://b.example/"sv) }, { 0, 1 }, 1));
        initialize_navigable_entry_identities();
    }

    void with_two_same_document_top_level_entries()
    {
        auto first_entry = entry(0, "https://a.example/first"sv);
        auto second_entry = entry(1, "https://a.example/second"sv);
        second_entry.document_state.id = first_entry.document_state.id;
        VERIFY(history.initialize_for_testing({ move(first_entry), move(second_entry) }, { 0, 1 }, 1));
        initialize_navigable_entry_identities();
    }

    // Three top-level entries; the current entry is the second.
    void with_three_top_level_entries()
    {
        VERIFY(history.initialize_for_testing({ entry(0, "https://a.example/"sv), entry(1, "https://b.example/"sv), entry(2, "https://c.example/"sv) }, { 0, 1, 2 }, 1));
        initialize_navigable_entry_identities();
    }

    // One top-level entry whose document holds a child navigable that has pushed an entry (step 1), followed by a
    // later top-level entry (step 2). The current step is the child's pushed entry.
    void with_child_navigable_history()
    {
        add_child(child_id());
        auto top0 = entry(0, "https://top.example/0"sv);
        top0.document_state.nested_histories.append({
            .id = child_id(),
            .entries = { entry(0, "https://child.example/0"sv), entry(1, "https://child.example/1"sv) },
        });
        VERIFY(history.initialize_for_testing({ move(top0), entry(2, "https://top.example/1"sv) }, { 0, 1, 2 }, 1));
        initialize_navigable_entry_identities();
    }

    void with_finalized_cross_document_replacement()
    {
        auto initial_entry = entry(0, "https://a.example/"sv);
        VERIFY(history.initialize_for_testing({ initial_entry }, { 0 }, 0));
        initialize_navigable_entry_identities();

        auto replacement_entry = entry(0, "https://b.example/"sv);
        VERIFY(history.finalize_cross_document_navigation({}, pending_entry(move(replacement_entry)), initial_entry.navigation_api_key).has_value());
    }

    WebView::ApplyHistoryStep& apply_step(i32 step, Optional<Web::Bindings::NavigationType> navigation_type, bool check_for_cancelation = false, Optional<Web::HTML::CrossProcessId> initiator_to_check = {}, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot = {})
    {
        operation = make<WebView::ApplyHistoryStep>(history, traversable, queue, state, runner.jobs(), step,
            check_for_cancelation, initiator_to_check, initiator_source_snapshot, Web::HTML::UserNavigationInvolvement::BrowserUI,
            navigation_type,
            [this](Web::HTML::HistoryStepResult history_step_result) { result = history_step_result; });
        operation->apply_the_history_step();
        return *operation;
    }

    WebView::ApplyHistoryStep& traverse_to_step(i32 step, bool check_for_cancelation = false, Optional<Web::HTML::CrossProcessId> initiator_to_check = {}, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot = {})
    {
        return apply_step(step, Web::Bindings::NavigationType::Traverse, check_for_cancelation, initiator_to_check, initiator_source_snapshot);
    }

    Optional<i32> current_step() const
    {
        if (!history.current_used_step_index().has_value())
            return {};
        return history.step_at(*history.current_used_step_index());
    }

    void initialize_navigable_entry_identities()
    {
        auto step = current_step();
        VERIFY(step.has_value());
        traversable.for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable& navigable) {
            auto const* current_entry = history.get_the_target_history_entry(navigable, *step);
            VERIFY(current_entry);
            navigable.set_current_session_history_entry(*current_entry);
            navigable.set_active_session_history_entry(*current_entry);
            return IterationDecision::Continue;
        });
    }

    WebView::TraversableSessionHistory history;
    WebView::CanonicalTraversable traversable;
    WebView::SessionHistoryTraversalQueue queue;
    WebView::TraversableApplyHistoryStepState state;
    FakeJobRunner runner;
    OwnPtr<WebView::ApplyHistoryStep> operation;
    Optional<Web::HTML::HistoryStepResult> result;
};

}

TEST_CASE(finalized_replacement_is_selected_from_the_navigables_current_entry)
{
    TestTraversable test;
    test.with_finalized_cross_document_replacement();

    test.apply_step(0, Web::Bindings::NavigationType::Replace);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    EXPECT_EQ(test.runner.changing_jobs[0].job.navigable_id, root_id());
    EXPECT_EQ(test.runner.changing_jobs[0].job.target_entry.url, parse_url("https://b.example/"sv));
}

TEST_CASE(traversal_runs_the_changing_root_job_and_commits_the_target_step)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    auto& operation = test.traverse_to_step(0);
    EXPECT(test.runner.unload_cancelation_jobs.is_empty());

    EXPECT_EQ(test.runner.selected_changing_job_endpoints.size(), 1uz);
    EXPECT_EQ(test.runner.selected_changing_job_endpoints[0], root_id());
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto const* target_entry = test.history.get_the_target_history_entry(test.traversable, 0);
    VERIFY(target_entry);
    EXPECT(test.traversable.current_session_history_entry_is(*target_entry));
    EXPECT(!test.traversable.active_document_is(*target_entry));
    auto& job = test.runner.changing_jobs[0];
    EXPECT_EQ(job.job.navigable_id, root_id());
    EXPECT_EQ(job.job.target_entry.url, parse_url("https://a.example/"sv));
    EXPECT_EQ(job.job.navigation_api_abort_behavior, Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Abort);
    job.on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);

    EXPECT_EQ(test.runner.continuations.size(), 1uz);
    auto& continuation = test.runner.continuations[0];
    EXPECT_EQ(continuation.continuation.navigable_id, root_id());
    EXPECT_EQ(continuation.continuation.history_object_length_and_index.script_history_length, 2u);
    EXPECT_EQ(continuation.continuation.history_object_length_and_index.script_history_index, 0u);
    EXPECT_EQ(continuation.continuation.entries_for_navigation_api.size(), 1uz);
    continuation.on_complete();

    EXPECT(test.runner.nonchanging_updates.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 0);
    EXPECT(operation.completed());
}

TEST_CASE(same_document_traversal_preserves_the_navigation_api_event)
{
    TestTraversable test;
    test.with_two_same_document_top_level_entries();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    EXPECT_EQ(test.runner.changing_jobs[0].job.navigation_api_abort_behavior, Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Preserve);
}

TEST_CASE(canceled_unloading_returns_before_any_changing_jobs)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    test.traverse_to_step(0, true);
    EXPECT_EQ(test.runner.unload_cancelation_jobs.size(), 1uz);
    auto const* target_entry = test.history.get_the_target_history_entry(test.traversable, 0);
    VERIFY(target_entry);
    EXPECT(!test.traversable.current_session_history_entry_is(*target_entry));
    auto& job = test.runner.unload_cancelation_jobs[0];
    EXPECT_EQ(job.target_entry.step, 0);
    EXPECT_EQ(job.target_entry.url, parse_url("https://a.example/"sv));
    EXPECT_EQ(job.navigables_crossing_documents.size(), 1uz);
    EXPECT_EQ(job.navigables_crossing_documents[0], root_id());
    job.on_complete(Web::HTML::HistoryStepResult::CanceledByBeforeUnload);

    EXPECT(test.runner.changing_jobs.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::CanceledByBeforeUnload);
    EXPECT_EQ(test.current_step(), 1);
    EXPECT(!test.traversable.current_session_history_entry_is(*target_entry));
}

TEST_CASE(disallowed_initiator_returns_before_the_cancelation_check)
{
    TestTraversable test;
    test.with_two_top_level_entries();
    test.add_child(child_id());

    test.traverse_to_step(0, true, child_id(),
        Web::InitiatorSourceSnapshot { .sandboxing_flags = Web::HTML::SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation, .has_transient_activation = false });

    EXPECT(test.runner.unload_cancelation_jobs.is_empty());
    EXPECT(test.runner.changing_jobs.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::InitiatorDisallowed);
    EXPECT_EQ(test.current_step(), 1);
}

TEST_CASE(allowed_initiator_proceeds_to_the_cancelation_check)
{
    TestTraversable test;
    test.with_two_top_level_entries();
    test.add_child(child_id());

    test.traverse_to_step(0, true, child_id(), Web::InitiatorSourceSnapshot {});

    EXPECT_EQ(test.runner.unload_cancelation_jobs.size(), 1uz);
    EXPECT(!test.result.has_value());
}

TEST_CASE(initiator_without_a_snapshot_fails_closed)
{
    TestTraversable test;
    test.with_two_top_level_entries();
    test.add_child(child_id());

    test.traverse_to_step(0, true, child_id());

    EXPECT(test.runner.unload_cancelation_jobs.is_empty());
    EXPECT(test.runner.changing_jobs.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::InitiatorDisallowed);
}

TEST_CASE(sandboxed_removed_initiator_is_disallowed)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    test.traverse_to_step(0, true, child_id(), Web::InitiatorSourceSnapshot { .sandboxing_flags = Web::HTML::SandboxingFlagSet::SandboxedNavigation });

    EXPECT(test.runner.unload_cancelation_jobs.is_empty());
    EXPECT(test.runner.changing_jobs.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::InitiatorDisallowed);
}

TEST_CASE(unsandboxed_removed_initiator_proceeds_to_the_cancelation_check)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    test.traverse_to_step(0, true, child_id(), Web::InitiatorSourceSnapshot {});

    EXPECT_EQ(test.runner.unload_cancelation_jobs.size(), 1uz);
    EXPECT(!test.result.has_value());
}

TEST_CASE(child_navigable_traversal_updates_the_nonchanging_root)
{
    TestTraversable test;
    test.with_child_navigable_history();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto& job = test.runner.changing_jobs[0];
    EXPECT_EQ(job.job.navigable_id, child_id());
    EXPECT_EQ(job.job.target_entry.url, parse_url("https://child.example/0"sv));
    job.on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);

    EXPECT_EQ(test.runner.continuations.size(), 1uz);
    auto& continuation = test.runner.continuations[0];
    EXPECT_EQ(continuation.continuation.navigable_id, child_id());
    EXPECT_EQ(continuation.continuation.history_object_length_and_index.script_history_length, 3u);
    EXPECT_EQ(continuation.continuation.history_object_length_and_index.script_history_index, 0u);
    EXPECT_EQ(continuation.continuation.entries_for_navigation_api.size(), 2uz);
    continuation.on_complete();

    EXPECT_EQ(test.runner.nonchanging_updates.size(), 1uz);
    auto& nonchanging_update = test.runner.nonchanging_updates[0];
    EXPECT_EQ(nonchanging_update.navigable_id, root_id());
    EXPECT_EQ(nonchanging_update.history_object_length_and_index.script_history_index, 0u);
    EXPECT(!test.result.has_value());
    nonchanging_update.on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 0);
}

TEST_CASE(skipped_changing_job_still_applies_the_history_step)
{
    TestTraversable test;
    test.with_child_navigable_history();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto& child = *test.traversable.children().first();
    auto const* target_entry = test.history.get_the_target_history_entry(child, 0);
    VERIFY(target_entry);
    EXPECT(child.current_session_history_entry_is(*target_entry));
    EXPECT(!child.active_document_is(*target_entry));
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);

    EXPECT(test.runner.continuations.is_empty());
    EXPECT_EQ(test.runner.nonchanging_updates.size(), 1uz);
    test.runner.nonchanging_updates[0].on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 0);
}

TEST_CASE(stale_changing_job_completes_without_committing_the_target_step)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Stale);

    EXPECT(test.runner.continuations.is_empty());
    EXPECT(test.runner.nonchanging_updates.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 1);
}

TEST_CASE(synchronous_navigation_steps_jump_the_queue_before_continuations)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_two_top_level_entries();

    bool synchronous_steps_ran = false;
    RefPtr<Core::Promise<Empty>> synchronous_steps_signal;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        synchronous_steps_ran = true;
        synchronous_steps_signal = signal;
    });

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    EXPECT(!synchronous_steps_ran);
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);

    // The queued synchronous navigation ran before the continuation was applied, and pauses this run until it
    // signals completion.
    EXPECT(synchronous_steps_ran);
    EXPECT(test.state.running_nested_apply_history_step);
    EXPECT(test.runner.continuations.is_empty());

    synchronous_steps_signal->resolve({});
    EXPECT(!test.state.running_nested_apply_history_step);
    EXPECT_EQ(test.runner.continuations.size(), 1uz);

    // A synchronous navigation targeting a navigable that has already been traversed no longer jumps the queue.
    bool late_synchronous_steps_ran = false;
    test.queue.append_session_history_synchronous_navigation_steps(root_id(), [&](NonnullRefPtr<Core::Promise<Empty>>) {
        late_synchronous_steps_ran = true;
    });
    test.runner.continuations[0].on_complete();

    EXPECT(!late_synchronous_steps_ran);
    EXPECT(!test.queue.is_empty());
    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 0);
}

TEST_CASE(synchronous_navigation_steps_queued_by_a_nested_step_do_not_starve_the_continuation)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_two_top_level_entries();

    bool later_synchronous_steps_ran = false;
    RefPtr<Core::Promise<Empty>> synchronous_steps_signal;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        synchronous_steps_signal = signal;
        test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>>) {
            later_synchronous_steps_ran = true;
        });
    });

    test.traverse_to_step(0);
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT(synchronous_steps_signal);
    EXPECT(test.runner.continuations.is_empty());

    synchronous_steps_signal->resolve({});
    EXPECT(!later_synchronous_steps_ran);
    EXPECT_EQ(test.runner.continuations.size(), 1uz);
    EXPECT(!test.queue.is_empty());
}

TEST_CASE(canceled_run_does_not_resume_after_synchronous_navigation)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_two_top_level_entries();

    RefPtr<Core::Promise<Empty>> synchronous_steps_signal;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        synchronous_steps_signal = signal;
    });

    test.traverse_to_step(0);
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT(synchronous_steps_signal);
    EXPECT(test.state.running_nested_apply_history_step);

    test.operation = nullptr;
    EXPECT(!test.state.running_nested_apply_history_step);

    synchronous_steps_signal->resolve({});
    EXPECT(test.runner.continuations.is_empty());
}

TEST_CASE(an_older_run_does_not_commit_over_a_newer_runs_step)
{
    TestTraversable test;
    test.with_three_top_level_entries();

    Optional<Web::HTML::HistoryStepResult> older_result;
    WebView::ApplyHistoryStep older_operation(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), 0,
        false, {}, {}, Web::HTML::UserNavigationInvolvement::BrowserUI, Web::Bindings::NavigationType::Traverse,
        [&](Web::HTML::HistoryStepResult result) { older_result = result; });

    // A newer run (for example a synchronous navigation that jumped the queue) commits first.
    test.traverse_to_step(2);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), 1uz);
    test.runner.continuations[0].on_complete();
    EXPECT_EQ(test.current_step(), 2);

    older_operation.apply_the_history_step();
    EXPECT_EQ(test.runner.changing_jobs.size(), 2uz);
    test.runner.changing_jobs[1].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), 2uz);
    test.runner.continuations[1].on_complete();

    EXPECT(older_result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 2);
}
