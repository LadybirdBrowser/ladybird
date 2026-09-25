/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/StringHash.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>

static Web::HTML::CrossProcessId root_id() { return { 1, 1 }; }
static Web::HTML::CrossProcessId child_id() { return { 1, 2 }; }
static Web::HTML::CrossProcessId first_operation_id() { return { 2, 1 }; }
static Web::HTML::CrossProcessId second_operation_id() { return { 2, 2 }; }
static Web::HTML::CrossProcessId third_operation_id() { return { 2, 3 }; }

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

// Each test entry has a document state of its own, unless a test gives it one.
static u64 s_next_test_document_state_local_id = 1000000;

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url)
{
    auto parsed_url = parse_url(url);
    return {
        .step = step,
        .url = parsed_url,
        .document_state = {
            .id = { 3, s_next_test_document_state_local_id++ },
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
            .queue_navigation_api_state_clear_task = [this](Web::HTML::CrossProcessId navigable_id) { navigation_api_state_clear_tasks.append(navigable_id); },
            .select_changing_navigable_history_step_job_endpoint = [this](ChangingNavigableHistoryStepJob& job) {
                selected_changing_job_endpoints.append(job.navigable_id);
                return true; },
            .run_changing_navigable_history_step_job = [this](ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) { changing_jobs.append({ move(job), move(on_complete) }); },
            .apply_changing_navigable_history_step_continuation = [this](ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) { continuations.append({ move(continuation), move(on_complete) }); },
            .update_nonchanging_navigable_history_step_state = [this](Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete) { nonchanging_updates.append({ navigable_id, history_object_length_and_index, move(on_complete) }); },
        };
    }

    Vector<UnloadCancelationJob> unload_cancelation_jobs;
    Vector<Web::HTML::CrossProcessId> navigation_api_state_clear_tasks;
    Vector<Web::HTML::CrossProcessId> selected_changing_job_endpoints;
    Vector<ChangingJob> changing_jobs;
    Vector<Continuation> continuations;
    Vector<NonchangingUpdate> nonchanging_updates;
};

struct TestTraversable {
    TestTraversable()
    {
        traversable.set_id(root_id());
        traversable.set_active_document_state(WebView::CanonicalDocumentState::create({}, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {}).document));
    }

    WebView::CanonicalNavigable& add_child(Web::HTML::CrossProcessId id)
    {
        return traversable.append_child(make<WebView::CanonicalNavigable>(id, RefPtr<WebView::WebContentPage> {}));
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

    void with_two_changing_navigables()
    {
        add_child(child_id());

        auto top0 = entry(0, "https://top.example/0"sv);
        top0.document_state.nested_histories.append({
            .id = child_id(),
            .entries = { entry(0, "https://child.example/0"sv), entry(2, "https://child.example/2"sv) },
        });

        auto top2 = entry(2, "https://top.example/2"sv);
        top2.document_state.id = top0.document_state.id;
        VERIFY(history.initialize_for_testing({ move(top0), move(top2) }, { 0, 2 }, 1));
        initialize_navigable_entry_identities();
    }

    void with_finalized_cross_document_replacement()
    {
        auto initial_entry = entry(0, "https://a.example/"sv);
        VERIFY(history.initialize_for_testing({ initial_entry }, { 0 }, 0));
        initialize_navigable_entry_identities();

        auto* current_entry = history.current_entry();
        VERIFY(current_entry);
        VERIFY(history.replace_session_history_entry(traversable, *current_entry, MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(entry(0, "https://b.example/"sv)))));
    }

    WebView::ApplyHistoryStep& apply_step(i32 step, Optional<Web::Bindings::NavigationType> navigation_type, bool check_for_cancelation = false, Optional<Web::HTML::CrossProcessId> initiator_to_check = {}, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot = {})
    {
        operation = make<WebView::ApplyHistoryStep>(history, traversable, queue, state, runner.jobs(), second_operation_id(), 2, step,
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
            auto* current_entry = history.get_the_target_history_entry(navigable, *step);
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

TEST_CASE(ongoing_traversal_is_owned_by_its_history_operation)
{
    WebView::CanonicalNavigable navigable(root_id(), {});

    navigable.set_ongoing_navigation_to_traversal(first_operation_id());
    navigable.set_ongoing_navigation_to_traversal(second_operation_id());
    navigable.clear_ongoing_navigation_traversal(first_operation_id());
    EXPECT(navigable.ongoing_navigation_is_traversal());

    navigable.clear_ongoing_navigation_traversal(second_operation_id());
    EXPECT(!navigable.ongoing_navigation_is_traversal());

    navigable.set_ongoing_navigation_to_traversal(first_operation_id());
    navigable.clear_ongoing_navigation();
    EXPECT(!navigable.ongoing_navigation_is_traversal());
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
    auto* target_entry = test.history.get_the_target_history_entry(test.traversable, 0);
    VERIFY(target_entry);
    EXPECT(test.traversable.current_session_history_entry_is(*target_entry));
    EXPECT(!test.traversable.active_document_is(*target_entry));
    EXPECT_EQ(test.runner.navigation_api_state_clear_tasks.size(), 1uz);
    EXPECT_EQ(test.runner.navigation_api_state_clear_tasks[0], root_id());
    EXPECT(test.traversable.ongoing_navigation_is_traversal());
    auto& job = test.runner.changing_jobs[0];
    EXPECT_EQ(job.job.navigable_id, root_id());
    EXPECT_EQ(job.job.target_entry.url, parse_url("https://a.example/"sv));
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
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());
}

TEST_CASE(same_document_traversal_does_not_clear_the_navigation_api_state)
{
    TestTraversable test;
    test.with_two_same_document_top_level_entries();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    EXPECT(test.runner.navigation_api_state_clear_tasks.is_empty());
}

TEST_CASE(same_document_traversal_yields_to_a_navigation_awaiting_admission)
{
    TestTraversable test;
    test.with_two_same_document_top_level_entries();

    test.traverse_to_step(0);
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto const& job = test.runner.changing_jobs[0].job;
    EXPECT(job.traversal_yields_to == Web::HTML::TraversalYieldsTo::UnadmittedNavigation);
    EXPECT(!job.canceled_navigation_id.has_value());
}

TEST_CASE(same_document_traversal_names_the_older_navigation_it_cancels)
{
    TestTraversable test;
    test.with_two_same_document_top_level_entries();
    test.traversable.set_ongoing_navigation({ .navigation_id = "older"_utf16, .sequence_number = 1 });

    test.traverse_to_step(0);
    EXPECT(!test.traversable.ongoing_navigation().has_value());
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto const& job = test.runner.changing_jobs[0].job;
    EXPECT(job.traversal_yields_to == Web::HTML::TraversalYieldsTo::UnadmittedNavigation);
    EXPECT_EQ(job.canceled_navigation_id, "older"_utf16);
}

TEST_CASE(same_document_traversal_yields_to_a_newer_admitted_navigation)
{
    TestTraversable test;
    test.with_two_same_document_top_level_entries();
    test.traversable.set_ongoing_navigation({ .navigation_id = "newer"_utf16, .sequence_number = 3 });

    test.traverse_to_step(0);
    EXPECT(test.traversable.ongoing_navigation().has_value());
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto const& job = test.runner.changing_jobs[0].job;
    EXPECT(job.traversal_yields_to == Web::HTML::TraversalYieldsTo::AdmittedNavigation);
    EXPECT(!job.canceled_navigation_id.has_value());
}

TEST_CASE(cross_document_traversal_cancels_a_newer_navigation)
{
    TestTraversable test;
    test.with_two_top_level_entries();
    test.traversable.set_ongoing_navigation({ .navigation_id = "newer"_utf16, .sequence_number = 3 });

    test.traverse_to_step(0);
    EXPECT(!test.traversable.ongoing_navigation().has_value());
    EXPECT(test.traversable.ongoing_navigation_is_traversal());
    EXPECT_EQ(test.runner.changing_jobs.size(), 1uz);
    auto const& job = test.runner.changing_jobs[0].job;
    EXPECT(job.traversal_yields_to == Web::HTML::TraversalYieldsTo::Nothing);
    EXPECT(!job.canceled_navigation_id.has_value());
}

TEST_CASE(canceled_unloading_returns_before_any_changing_jobs)
{
    TestTraversable test;
    test.with_two_top_level_entries();

    test.traverse_to_step(0, true);
    EXPECT_EQ(test.runner.unload_cancelation_jobs.size(), 1uz);
    auto* target_entry = test.history.get_the_target_history_entry(test.traversable, 0);
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
    auto* target_entry = test.history.get_the_target_history_entry(child, 0);
    VERIFY(target_entry);
    EXPECT(child.current_session_history_entry_is(*target_entry));
    EXPECT(!child.active_document_is(*target_entry));
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);

    EXPECT(test.runner.continuations.is_empty());
    EXPECT_EQ(test.runner.nonchanging_updates.size(), 1uz);
    test.runner.nonchanging_updates[0].on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 0);
    EXPECT(!child.ongoing_navigation_is_traversal());
}

TEST_CASE(ready_continuations_interleave_with_still_running_changing_jobs)
{
    TestTraversable test;
    test.with_two_changing_navigables();

    test.traverse_to_step(0);
    EXPECT_EQ(test.runner.changing_jobs.size(), 2uz);

    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), 1uz);

    test.runner.changing_jobs[1].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), 1uz);

    test.runner.continuations[0].on_complete();
    EXPECT_EQ(test.runner.continuations.size(), 2uz);
    EXPECT(!test.result.has_value());

    test.runner.continuations[1].on_complete();
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
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());
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
    EXPECT(synchronous_steps_ran);
    EXPECT(test.state.running_nested_apply_history_step);
    EXPECT(test.runner.continuations.is_empty());

    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
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
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());

    synchronous_steps_signal->resolve({});
    EXPECT(test.runner.continuations.is_empty());
}

TEST_CASE(an_older_run_does_not_commit_over_a_newer_runs_step)
{
    TestTraversable test;
    test.with_three_top_level_entries();

    Optional<Web::HTML::HistoryStepResult> older_result;
    WebView::ApplyHistoryStep older_operation(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), first_operation_id(), 1, 0,
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

TEST_CASE(a_paused_run_commits_after_a_newer_run_recommits_the_current_step)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_two_top_level_entries();

    // A push whose finalization appended its entry at step 2.
    VERIFY(test.history.push_session_history_entry(test.traversable, MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(entry(2, "https://c.example/"sv)))) == 2);

    // A synchronous replace from the page the push is unloading is queued behind the push and jumps the queue while the
    // push waits on its changing job. It re-commits the current step, moving nothing.
    OwnPtr<WebView::ApplyHistoryStep> nested_operation;
    Optional<Web::HTML::HistoryStepResult> nested_result;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        nested_operation = make<WebView::ApplyHistoryStep>(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), third_operation_id(), 3, 1,
            false, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::InitiatorSourceSnapshot> {}, Web::HTML::UserNavigationInvolvement::None, Web::Bindings::NavigationType::Replace,
            [&, signal](Web::HTML::HistoryStepResult result) {
                nested_result = result;
                signal->resolve({});
            });
        nested_operation->apply_the_history_step();
    });

    test.apply_step(2, Web::Bindings::NavigationType::Push);
    EXPECT(!test.runner.changing_jobs.is_empty());
    EXPECT(nested_operation);

    // Complete whatever the nested run dispatched; the push's own job (the first) stays pending.
    for (size_t i = 1; i < test.runner.changing_jobs.size(); ++i)
        test.runner.changing_jobs[i].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    for (size_t i = 0; i < test.runner.continuations.size(); ++i)
        test.runner.continuations[i].on_complete();
    EXPECT(nested_result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(nested_operation->committed_step(), 1);
    EXPECT_EQ(test.current_step(), 1);
    EXPECT(!test.state.running_nested_apply_history_step);

    // The push resumes, and commits its own step.
    auto continuation_count = test.runner.continuations.size();
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), continuation_count + 1);
    test.runner.continuations[continuation_count].on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.operation->committed_step(), 2);
    EXPECT_EQ(test.current_step(), 2);
}

TEST_CASE(a_paused_traversal_is_abandoned_once_a_jumping_push_removes_its_target_entry)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_three_top_level_entries();

    // A same-document push from the current page is queued behind a forward traversal, and jumps the queue while the
    // traversal waits on its changing job. It clears the forward entry that the traversal has claimed.
    OwnPtr<WebView::ApplyHistoryStep> nested_operation;
    Optional<Web::HTML::HistoryStepResult> nested_result;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        auto pushed_entry = WebView::CanonicalSessionHistoryEntry::create(test.history.entry_for_step(1)->document_state);
        pushed_entry->url = parse_url("https://b.example/pushed"sv);
        VERIFY(test.history.push_session_history_entry(test.traversable, move(pushed_entry)) == 2);
        nested_operation = make<WebView::ApplyHistoryStep>(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), third_operation_id(), 3, 2,
            false, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::InitiatorSourceSnapshot> {}, Web::HTML::UserNavigationInvolvement::None, Web::Bindings::NavigationType::Push,
            [&, signal](Web::HTML::HistoryStepResult result) {
                nested_result = result;
                signal->resolve({});
            });
        nested_operation->apply_the_history_step();
    });

    test.traverse_to_step(2);
    EXPECT(!test.runner.changing_jobs.is_empty());
    EXPECT_EQ(test.runner.changing_jobs[0].job.target_entry.url, parse_url("https://c.example/"sv));
    EXPECT(nested_operation);

    // Complete whatever the nested run dispatched; the traversal's own job (the first) stays pending.
    for (size_t i = 1; i < test.runner.changing_jobs.size(); ++i)
        test.runner.changing_jobs[i].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    for (size_t i = 0; i < test.runner.continuations.size(); ++i)
        test.runner.continuations[i].on_complete();
    EXPECT(nested_result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 2);

    // The traversal's job reports in, but its target entry is gone: No continuation is applied, and the push's entry
    // stays current.
    auto continuation_count = test.runner.continuations.size();
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), continuation_count);
    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT(!test.operation->committed_step().has_value());
    EXPECT_EQ(test.current_step(), 2);
    EXPECT_EQ(test.history.current_entry()->url, parse_url("https://b.example/pushed"sv));
    EXPECT(!test.traversable.ongoing_navigation_is_traversal());
}

TEST_CASE(a_paused_push_appends_its_entry_again_once_a_jumping_push_clears_it)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_three_top_level_entries();

    // A cross-document navigation from the page at step 1 has been finalized: Its entry took the place of the forward
    // history, and its push waits on its changing job.
    auto navigation_entry = MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(entry(2, "https://d.example/"sv)));
    VERIFY(test.history.push_session_history_entry(test.traversable, navigation_entry) == 2);

    // A same-document push from the page being navigated away from is queued behind the navigation, and jumps the queue
    // while the navigation waits. It clears the navigation's entry as forward history, and takes its step.
    OwnPtr<WebView::ApplyHistoryStep> nested_operation;
    Optional<Web::HTML::HistoryStepResult> nested_result;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        auto pushed_entry = WebView::CanonicalSessionHistoryEntry::create(test.history.entry_for_step(1)->document_state);
        pushed_entry->url = parse_url("https://b.example/pushed"sv);
        VERIFY(test.history.push_session_history_entry(test.traversable, move(pushed_entry)) == 2);
        nested_operation = make<WebView::ApplyHistoryStep>(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), third_operation_id(), 3, 2,
            false, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::InitiatorSourceSnapshot> {}, Web::HTML::UserNavigationInvolvement::None, Web::Bindings::NavigationType::Push,
            [&, signal](Web::HTML::HistoryStepResult result) {
                nested_result = result;
                signal->resolve({});
            });
        nested_operation->apply_the_history_step();
    });

    test.apply_step(2, Web::Bindings::NavigationType::Push);
    EXPECT(!test.runner.changing_jobs.is_empty());
    EXPECT_EQ(test.runner.changing_jobs[0].job.target_entry.url, parse_url("https://d.example/"sv));
    EXPECT(nested_operation);

    // Complete whatever the nested run dispatched; the navigation's own job (the first) stays pending.
    for (size_t i = 1; i < test.runner.changing_jobs.size(); ++i)
        test.runner.changing_jobs[i].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    for (size_t i = 0; i < test.runner.continuations.size(); ++i)
        test.runner.continuations[i].on_complete();
    EXPECT(nested_result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 2);

    // The navigation's job reports in. Its entry goes back into the session history, after the pushed entry, and the
    // navigation commits there.
    auto continuation_count = test.runner.continuations.size();
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), continuation_count + 1);
    auto& continuation = test.runner.continuations[continuation_count].continuation;
    VERIFY(continuation.updated_target_entry.has_value());
    EXPECT_EQ(continuation.updated_target_entry->url, navigation_entry->url);
    EXPECT_EQ(continuation.updated_target_entry->navigation_api_id, navigation_entry->navigation_api_id);
    EXPECT_EQ(continuation.updated_target_entry->step, 3);
    test.runner.continuations[continuation_count].on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.operation->committed_step(), 3);
    EXPECT_EQ(test.current_step(), 3);
    EXPECT_EQ(test.history.current_entry()->url, navigation_entry->url);
    auto entries = test.history.get_session_history_entries(test.traversable);
    VERIFY(entries.has_value());
    EXPECT_EQ(entries->size(), 4u);
    EXPECT_EQ(entries->at(2)->url, parse_url("https://b.example/pushed"sv));
}

TEST_CASE(a_paused_reload_continues_with_the_entry_that_a_jumping_replace_put_in_its_targets_slot)
{
    Core::EventLoop event_loop;
    TestTraversable test;
    test.with_two_top_level_entries();
    test.history.mark_current_entry_reload_pending();

    // A same-document replace from the page being reloaded is queued behind the reload, and jumps the queue while the
    // reload waits on its changing job. It replaces the entry that the reload has claimed, keeping its navigation API
    // key and its document state.
    RefPtr reloading_entry = test.history.current_entry();
    auto replacement_entry = WebView::CanonicalSessionHistoryEntry::create(reloading_entry->document_state);
    replacement_entry->url = parse_url("https://b.example/?replaced"sv);
    replacement_entry->navigation_api_key = reloading_entry->navigation_api_key;
    replacement_entry->navigation_api_id = "https://b.example/?replaced"_utf16;

    OwnPtr<WebView::ApplyHistoryStep> nested_operation;
    Optional<Web::HTML::HistoryStepResult> nested_result;
    test.queue.append_session_history_synchronous_navigation_steps(child_id(), [&](NonnullRefPtr<Core::Promise<Empty>> signal) {
        VERIFY(test.history.replace_session_history_entry(test.traversable, *reloading_entry, replacement_entry));
        nested_operation = make<WebView::ApplyHistoryStep>(test.history, test.traversable, test.queue, test.state, test.runner.jobs(), third_operation_id(), 3, 1,
            false, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::InitiatorSourceSnapshot> {}, Web::HTML::UserNavigationInvolvement::None, Web::Bindings::NavigationType::Replace,
            [&, signal](Web::HTML::HistoryStepResult result) {
                nested_result = result;
                signal->resolve({});
            });
        nested_operation->apply_the_history_step();
    });

    test.apply_step(1, Web::Bindings::NavigationType::Reload);
    EXPECT(!test.runner.changing_jobs.is_empty());
    EXPECT_EQ(test.runner.changing_jobs[0].job.target_entry.navigation_api_id, reloading_entry->navigation_api_id);
    EXPECT(nested_operation);

    // Complete whatever the nested run dispatched; the reload's own job (the first) stays pending.
    for (size_t i = 1; i < test.runner.changing_jobs.size(); ++i)
        test.runner.changing_jobs[i].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    for (size_t i = 0; i < test.runner.continuations.size(); ++i)
        test.runner.continuations[i].on_complete();
    EXPECT(nested_result == Web::HTML::HistoryStepResult::Applied);

    // The reload resumes, and its continuation names the replacement as the entry to activate.
    auto continuation_count = test.runner.continuations.size();
    test.runner.changing_jobs[0].on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready);
    EXPECT_EQ(test.runner.continuations.size(), continuation_count + 1);
    auto const& continuation = test.runner.continuations[continuation_count].continuation;
    VERIFY(continuation.updated_target_entry.has_value());
    EXPECT_EQ(continuation.updated_target_entry->navigation_api_id, replacement_entry->navigation_api_id);
    EXPECT_EQ(continuation.updated_target_entry->url, replacement_entry->url);
    test.runner.continuations[continuation_count].on_complete();

    EXPECT(test.result == Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(test.current_step(), 1);
}
