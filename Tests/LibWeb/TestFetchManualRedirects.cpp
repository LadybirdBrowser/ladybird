/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Fetch ids in the recorded commands are numbered from 1 in the order the case started them. Request ids are the
// stand-in RequestServer client's, which start at 0 in each case.

#include "FetchTestFixtures.h"
#include <LibGC/Testing.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>

using Web::Fetch::Engine::Command::ResolveRedirect;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchControllerToken;

using namespace FetchTest;

static URL::URL parse_url(StringView url_string)
{
    auto url = URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    return url.release_value();
}

static HTTP::Header location(StringView value)
{
    return { "Location"sv, value };
}

static Vector<ByteString> describe_responses(AlgorithmCalls const& record)
{
    Vector<ByteString> responses;
    for (auto const& response : record.responses) {
        if (response->is_aborted_network_error())
            responses.append("aborted network error");
        else if (response->is_network_error())
            responses.append(ByteString::formatted("network error: {}", response->network_error_message().value_or({})));
        else
            responses.append(ByteString::number(response->status()));
    }
    return responses;
}

static ByteString describe_completion(Web::HTML::NavigationPopulationResult const& result)
{
    return result.navigation_params.visit(
        [](Web::HTML::NavigationParamsNullOrError const& error) {
            return error.has_value() ? ByteString::formatted("null: {}", *error) : ByteString { "null"sv };
        },
        [](Web::HTML::NavigationParamsDescriptor const&) { return ByteString { "navigation params"sv }; },
        [](Web::HTML::NonFetchSchemeNavigationParamsDescriptor const& params) { return ByteString::formatted("non-fetch scheme navigation params for {}", params.url); });
}

namespace {

// Where a navigation comes from: by default the browser's UI, with no fetch client.
struct NavigationSource {
    Web::HTML::NavigationSourceSnapshot snapshot { Web::HTML::create_navigation_source_snapshot_without_a_source_document() };
    Web::ContentSecurityPolicy::Directives::NavigationType csp_navigation_type { Web::ContentSecurityPolicy::Directives::NavigationType::Other };
    Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::BrowserUI };
};

}

// Navigates the navigable, by default the harness's traversable, to the URL through LocalNavigable's navigation fetch
// loop, as the UI process has a process populate a pending entry's document. Each result the navigation completes with
// is a step of completions.
static void start_navigation(FetchHarness const& harness, NonnullRefPtr<ReachedSteps> const& completions, StringView url, Utf16String const& navigation_id, NavigationSource source = {}, GC::Ptr<Web::HTML::LocalNavigable> navigable = {})
{
    if (!navigable)
        navigable = harness.document->navigable();
    VERIFY(navigable);
    navigable->set_ongoing_navigation_without_informing_navigation_api(navigation_id);

    auto& page_client = navigable->page().client();
    Web::HTML::PendingSessionHistoryEntryDescriptor entry;
    entry.url = parse_url(url);
    entry.document_state.id = page_client.allocate_cross_process_id();
    entry.directive_state_id = page_client.allocate_cross_process_id();
    Web::HTML::NavigationPopulationRequest request {
        .navigable_id = navigable->id(),
        .history_entry = move(entry),
        .source_snapshot_params = move(source.snapshot),
        .target_snapshot_params = {},
        .csp_navigation_type = source.csp_navigation_type,
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Push,
        .user_involvement = source.user_involvement,
        .navigation_id = navigation_id,
    };
    auto& realm = navigable->active_window()->principal_realm();
    auto source_snapshot_params = Web::HTML::create_source_snapshot_params_from_navigation_source_snapshot(realm, request.source_snapshot_params);
    auto response_steps = GC::create_function(realm.heap(), GC::captures(completions), [](auto& completions, Web::HTML::NavigationPopulationResult result) {
        completions->steps.append(describe_completion(result));
    });
    Web::HTML::TemporaryExecutionContext execution_context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    navigable->populate_session_history_entry_document(request, source_snapshot_params, false, response_steps);
}

static void run_until_completions(FetchHarness const& harness, NonnullRefPtr<ReachedSteps> const& completions, size_t count)
{
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(completions, count), [](auto& completions, auto& count) { return completions->steps.size() >= count; }));
    VERIFY(completions->steps.size() >= count);
}

// Another navigation of the traversable takes over, which a navigation still fetching sees as its ongoing navigation
// changing.
static void change_ongoing_navigation(FetchHarness const& harness, Utf16String const& navigation_id)
{
    harness.with_execution_context([&] {
        harness.document->navigable()->set_ongoing_navigation_without_informing_navigation_api(navigation_id);
    });
}

static NEVER_INLINE bool is_collected(FetchHarness const& harness, FetchControllerToken token)
{
    GC::scrub_stack_for_testing();
    harness.realm->heap().collect_garbage();
    return !find_controller(token);
}

TEST_CASE(a_followed_redirect_runs_its_next_hop_under_the_id_of_its_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/followed"sv, "followed"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/followed"sv);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302, { location("/followed-final"sv) });
    auto final_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(final_id, 200);
    run_until_completions(harness, completions, 1);
    harness.settle();

    // Following the redirect releases its request's lease and stops it, and the next hop's request starts.
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "navigation params" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "follow redirect of 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /followed", "release lease 0", "stop 0", "start 1 GET /followed-final" }));
    EXPECT(is_rooted(*token));
}

namespace {

// Sends an invalid follow before forwarding the navigation's real follow.
class InvalidFollowInjector final : public Web::Fetch::Engine::EngineCommandSink {
public:
    explicit InvalidFollowInjector(NonnullRefPtr<ReachedSteps> misbehaviors)
        : m_engine(Web::HTML::surrounding_agent().network_activity_observers, make<MisbehaviorRecordingAgent>(move(misbehaviors)))
    {
    }

    virtual void attach_to_executor(Web::Fetch::Engine::FetchExecutor& executor) override { m_engine.attach_to_executor(executor); }

    virtual void handle_command(Web::Fetch::Engine::EngineCommand command) override
    {
        if (auto* resolve = command.get_pointer<ResolveRedirect>()) {
            if (auto* follow = resolve->resolution.get_pointer<ResolveRedirect::Follow>()) {
                Web::Fetch::Engine::MainFetchInput next_hop { follow->next_hop.fetch_params, Web::Fetch::Infrastructure::Response::create() };
                m_engine.handle_command(ResolveRedirect { resolve->fetch_id, resolve->continuation_id, ResolveRedirect::Follow { move(next_hop) } });
            }
        }
        m_engine.handle_command(move(command));
    }

private:
    Web::Fetch::Fetching::FetchEngine m_engine;
};

}

TEST_CASE(a_rejected_follow_leaves_the_redirect_open)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto misbehaviors = make_ref_counted<ReachedSteps>();
    harness.run_fetches_on(make<InvalidFollowInjector>(misbehaviors));
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/rejected-follow"sv, "rejected-follow"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { location("/rejected-follow-final"sv) });
    auto final_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(final_id, 200);
    run_until_completions(harness, completions, 1);
    harness.settle();

    EXPECT_EQ(misbehaviors->steps, (Vector<ByteString> { "The agent gave main fetch a response other than a network error" }));
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "navigation params" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /rejected-follow", "release lease 0", "stop 0", "start 1 GET /rejected-follow-final" }));
}

TEST_CASE(a_redirect_without_a_location_is_accepted_as_the_navigations_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/final-redirect"sv, "final-redirect"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/final-redirect"sv);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302);
    run_until_completions(harness, completions, 1);
    harness.run_until_step(commands, "accept redirect of 1 as final"sv);
    harness.settle();

    EXPECT_EQ(completions->steps, (Vector<ByteString> { "navigation params" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "accept redirect of 1 as final" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /final-redirect" }));
    EXPECT(is_rooted(*token));

    // The fetch ends once the redirect's body has arrived.
    request_server.send_finished(redirect_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "accept redirect of 1 as final", "end of body of 1", "end of fetch 1" }));
    EXPECT(!is_rooted(*token));
}

// The navigation leaves at a redirect it does not follow, whose request has not finished.
static void leave_at_a_redirect(StringView path, StringView location_value, StringView expected_completion)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    auto url = ByteString::formatted("http://127.0.0.1:8000{}", path);
    start_navigation(harness, completions, url, "left"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for(path);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302, { location(location_value) });
    run_until_completions(harness, completions, 1);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    // The navigation abandons the redirect, which releases its request's lease and stops it, and the fetch ends. Nothing
    // else then keeps the fetch alive.
    EXPECT_EQ(completions->steps, (Vector<ByteString> { expected_completion }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { ByteString::formatted("start 0 GET {}", path), "release lease 0", "stop 0" }));
    EXPECT(!is_rooted(*token));
    harness.run_queued_invocations();
    EXPECT(is_collected(harness, *token));
}

TEST_CASE(a_navigation_that_leaves_at_an_invalid_location_abandons_the_redirect)
{
    leave_at_a_redirect("/invalid-location"sv, "http://["sv, "null"sv);
}

TEST_CASE(a_navigation_that_leaves_at_a_non_fetch_scheme_location_abandons_the_redirect)
{
    leave_at_a_redirect("/mailto-location"sv, "mailto:someone@example.com"sv, "non-fetch scheme navigation params for mailto:someone@example.com"sv);
}

TEST_CASE(a_navigation_that_leaves_at_a_data_location_abandons_the_redirect)
{
    leave_at_a_redirect("/data-location"sv, "data:text/html,hello"sv, "null"sv);
}

// The content navigable of an iframe inserted into the harness's document.
static Web::HTML::LocalNavigable& insert_iframe(FetchHarness const& harness)
{
    auto iframe = MUST(Web::DOM::create_element(*harness.document, "iframe"_utf16_fly_string, Web::Namespace::HTML));
    harness.with_execution_context([&] { MUST(harness.document->body()->append_child(iframe)); });
    return as<Web::HTML::LocalNavigable>(*as<Web::HTML::HTMLIFrameElement>(*iframe).content_navigable());
}

// The navigable's own document navigates it, so the navigation's fetch client is that document.
static NavigationSource navigation_source_of(Web::HTML::LocalNavigable& navigable)
{
    NavigationSource source { .user_involvement = Web::HTML::UserNavigationInvolvement::None };
    source.snapshot.fetch_client = navigable.active_document()->relevant_settings_object().serialize();
    return source;
}

// The navigation leaves at a redirect to a custom scheme, whose request has not finished.
TEST_CASE(an_iframe_navigation_that_leaves_at_a_custom_scheme_location_abandons_the_redirect)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    auto& iframe_navigable = insert_iframe(harness);
    start_navigation(harness, completions, "http://127.0.0.1:8000/iframe-redirect"sv, "iframe"_utf16, navigation_source_of(iframe_navigable), iframe_navigable);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/iframe-redirect"sv);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302, { location("web+custom:redirected"sv) });
    run_until_completions(harness, completions, 1);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    // The navigation abandons the redirect, which releases its request's lease and stops it, and the fetch ends. Nothing
    // else then keeps the fetch alive.
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "non-fetch scheme navigation params for web+custom:redirected" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /iframe-redirect", "release lease 0", "stop 0" }));
    EXPECT(!is_rooted(*token));
    harness.run_queued_invocations();
    EXPECT(is_collected(harness, *token));
}

// A form submission whose source's policy allows form actions on 127.0.0.1:8000 only. The check at the top of each
// iteration of the navigation's loop reads the request as the previous iteration's hop left it.
TEST_CASE(a_navigation_blocked_by_content_security_policy_at_its_next_hop_abandons_the_redirect)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    Web::ContentSecurityPolicy::SerializedPolicy form_action_policy {
        .directives = { { .name = "form-action"_utf16_fly_string, .value = { "http://127.0.0.1:8000"_utf16 } } },
        .disposition = Web::Bindings::SecurityPolicyViolationEventDisposition::Enforce,
        .source = Web::ContentSecurityPolicy::PolicySource::Header,
        .self_origin = parse_url("http://127.0.0.1:8000/"sv).origin(),
        .pre_parsed_policy_string = "form-action http://127.0.0.1:8000"_string,
    };
    NavigationSource source {
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::FormSubmission,
        .user_involvement = Web::HTML::UserNavigationInvolvement::None,
    };
    source.snapshot.fetch_client = harness.document->relevant_settings_object().serialize();
    source.snapshot.source_policy_container.csp_list.append(move(form_action_policy));
    start_navigation(harness, completions, "http://127.0.0.1:8000/form-action"sv, "form-action"_utf16, move(source));

    // The second iteration's check still reads the first URL, so the redirect to another host is followed.
    auto first_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/form-action"sv);
    VERIFY(token.has_value());
    request_server.send_headers(first_id, 302, { location("http://localhost:8000/form-action-other-host"sv) });
    auto second_id = request_server.wait_for_request("GET"sv);

    // The third iteration's check reads the other host, and blocks before the second redirect is followed.
    request_server.send_headers(second_id, 302, { location("/form-action-final"sv) });
    run_until_completions(harness, completions, 1);
    request_server.wait_for_message("stop 1"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null: Blocked by Content Security Policy" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "follow redirect of 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT(!is_rooted(*token));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /form-action", "release lease 0", "stop 0", "start 1 GET /form-action-other-host", "release lease 1", "stop 1" }));
}

// A redirect to another origin drops the request's reserved client, so the navigation's next iteration reads its parent
// document again, which is no longer active.
TEST_CASE(an_iframe_navigation_whose_parent_document_is_no_longer_active_abandons_the_redirect)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    auto& iframe_navigable = insert_iframe(harness);
    start_navigation(harness, completions, "http://127.0.0.1:8000/parent-gone"sv, "parent-gone"_utf16, navigation_source_of(iframe_navigable), iframe_navigable);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/parent-gone"sv);
    VERIFY(token.has_value());
    harness.document->navigable()->set_has_been_destroyed();
    request_server.send_headers(redirect_id, 302, { location("http://localhost:8000/parent-gone-other-origin"sv) });
    run_until_completions(harness, completions, 1);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null: Parent document is no longer active" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /parent-gone", "release lease 0", "stop 0" }));
    EXPECT(!is_rooted(*token));
}

TEST_CASE(a_redirect_abandoned_after_its_request_finished_has_its_lease_released)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/finished-redirect"sv, "finished-redirect"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/finished-redirect"sv);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302, { location("http://["sv) });
    request_server.send_finished(redirect_id);
    // The client hears that the request finished before the redirect is abandoned.
    request_server.deliver_sent_messages();
    run_until_completions(harness, completions, 1);
    request_server.wait_for_message("release lease 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    // A finished request is ended by the release of its lease alone.
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /finished-redirect", "release lease 0" }));
    EXPECT(!is_rooted(*token));
}

struct HeldFetch {
    GC::Root<FetchController> controller;
    FetchControllerToken token;
};

// These start and drive a fetch away from the test's own stack frame, so a conservative scan of it finds no controller.
static NEVER_INLINE HeldFetch start_a_navigation_fetch(FetchHarness const& harness, StringView url, NonnullRefPtr<AlgorithmCalls> const& record)
{
    auto request = harness.create_navigation_request_for(url);
    auto controller = harness.fetch_as_a_navigation(request, recording_responses(harness.realm->heap(), record));
    return { GC::make_root(controller), controller->token() };
}

static NEVER_INLINE void abandon_the_redirect_then_follow_it(FetchHarness const& harness, FetchControllerToken token)
{
    harness.with_execution_context([&] {
        auto controller = find_controller(token);
        controller->resolve_redirect(ResolveRedirect::Abandon {});
        controller->process_next_manual_redirect();
    });
}

// The navigation answered the redirect, as it does when it leaves its loop, and the redirect's steps run afterwards.
TEST_CASE(a_redirect_followed_after_it_was_answered_leaves_no_next_hop_behind)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto [controller, token] = start_a_navigation_fetch(harness, "http://127.0.0.1:8000/answered-redirect"sv, record);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { location("/answered-final"sv) });
    harness.run_until_responses(record, 1);

    abandon_the_redirect_then_follow_it(harness, token);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abandon redirect of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /answered-redirect", "release lease 0", "stop 0" }));
    EXPECT(!is_rooted(token));

    // No next hop waits for the engine, and nothing else keeps the fetch alive.
    record->responses.clear();
    controller = {};
    harness.run_queued_invocations();
    EXPECT(is_collected(harness, token));
}

// The redirect has been handed over, an abort lands and the engine handles it, and then the redirect's queued task
// follows the redirect, as a navigation's stale process response task does.
TEST_CASE(a_redirect_followed_after_its_fetch_was_aborted_runs_no_hop)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_navigation_request_for("http://127.0.0.1:8000/aborted-redirect"sv);
    auto controller = GC::make_root(harness.fetch_as_a_navigation(request, recording_responses(harness.realm->heap(), record)));
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { location("/aborted-final"sv) });
    harness.run_until_responses(record, 1);

    // The abort abandons the redirect the fetch waits on.
    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    request_server.wait_for_message("stop 0"sv);
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /aborted-redirect", "release lease 0", "stop 0" }));

    // The follow moves the request on to the next URL, but the engine runs no hop for the fetch, which has ended.
    harness.with_execution_context([&] { controller->process_next_manual_redirect(); });
    harness.run_until_step(commands, "follow redirect of 1"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1", "follow redirect of 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /aborted-redirect", "release lease 0", "stop 0" }));
    EXPECT_EQ(describe_responses(*record), (Vector<ByteString> { "302" }));
    EXPECT_EQ(request->redirect_count(), 1);
    EXPECT(!is_rooted(controller->token()));
}

// Another navigation takes over while the redirect's process response task is queued.
TEST_CASE(a_navigation_aborted_while_its_redirect_waits_gets_no_network_error)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/taken-over-redirect"sv, "taken-over"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/taken-over-redirect"sv);
    VERIFY(token.has_value());
    auto controller = GC::make_root(find_controller(*token));
    {
        // The event loop runs no task until the redirect has been handed over and another navigation has taken over.
        auto pause = harness.pause_event_loop();
        request_server.send_headers(redirect_id, 302, { location("/taken-over-final"sv) });
        harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(GC::Ref { *controller }), [](auto& controller) { return was_handed_over(*controller); }));

        // The redirect's process response task is queued, but has not run.
        VERIFY(was_handed_over(*controller));
        EXPECT_EQ(controller->original_fetch_params()->request()->redirect_count(), 0);
        EXPECT(completions->steps.is_empty());

        // The navigation aborts its fetch and completes with null at once.
        change_ongoing_navigation(harness, "taking-over"_utf16);
        EXPECT_EQ(completions->steps, (Vector<ByteString> { "null" }));
    }

    // The queued task still runs the loop and follows the redirect, which runs no hop, so no aborted network error
    // reaches the navigation.
    harness.run_until_step(commands, "follow redirect of 1"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1", "follow redirect of 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /taken-over-redirect", "release lease 0", "stop 0" }));
    EXPECT_EQ(controller->original_fetch_params()->request()->redirect_count(), 1);
    EXPECT(!is_rooted(controller->token()));
}

// The navigation follows 20 redirects, and HTTP-redirect fetch returns a network error for the 21st without running main
// fetch.
TEST_CASE(a_redirect_past_the_last_one_a_navigation_can_follow_is_abandoned)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/redirect-0"sv, "redirects"_utf16);
    Vector<ByteString> expected_commands { "start fetch 1" };
    Vector<ByteString> expected_messages;
    Optional<FetchControllerToken> token;
    for (size_t hop = 0; hop <= 20; ++hop) {
        auto request_id = request_server.wait_for_request("GET"sv);
        if (hop == 0)
            token = token_of_fetch_for("/redirect-0"sv);
        auto next_path = ByteString::formatted("/redirect-{}", hop + 1);
        request_server.send_headers(request_id, 302, { location(next_path) });
        expected_commands.append(hop < 20 ? "follow redirect of 1" : "abandon redirect of 1");
        expected_messages.append(ByteString::formatted("start {} GET /redirect-{}", request_id, hop));
        expected_messages.append(ByteString::formatted("release lease {}", request_id));
        expected_messages.append(ByteString::formatted("stop {}", request_id));
    }
    expected_commands.append("end of fetch 1");
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    // Nothing reaches the navigation, which waits until another one takes over.
    EXPECT(completions->steps.is_empty());
    EXPECT_EQ(commands->steps, expected_commands);
    EXPECT_EQ(request_server.messages(), expected_messages);
    VERIFY(token.has_value());
    EXPECT(!is_rooted(*token));
}

// A redirect is followed, and the fetch is aborted, stopped or taken over before the engine runs the next hop.
TEST_CASE(an_abort_before_the_engine_runs_a_followed_hop_hands_nothing_more_over)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_navigation_request_for("http://127.0.0.1:8000/abort-redirect"sv);
    auto controller = GC::make_root(harness.fetch_as_a_navigation(request, recording_responses(harness.realm->heap(), record)));
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { location("/abort-final"sv) });
    harness.run_until_responses(record, 1);

    harness.with_execution_context([&] {
        controller->process_next_manual_redirect();
        controller->abort(*harness.realm, {});
    });
    harness.run_until_step(commands, "abort 1"sv);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "follow redirect of 1", "abort 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /abort-redirect", "release lease 0", "stop 0" }));
    EXPECT_EQ(describe_responses(*record), (Vector<ByteString> { "302" }));
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_stop_before_the_engine_runs_a_followed_hop_starts_no_request)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_navigation_request_for("http://127.0.0.1:8000/stop-redirect"sv);
    auto controller = GC::make_root(harness.fetch_as_a_navigation(request, recording_responses(harness.realm->heap(), record)));
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { location("/stop-final"sv) });
    harness.run_until_responses(record, 1);

    harness.with_execution_context([&] {
        controller->process_next_manual_redirect();
        controller->stop_fetch();
    });
    harness.run_until_step(commands, "stop 1"sv);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "follow redirect of 1", "stop 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /stop-redirect", "release lease 0", "stop 0" }));
    EXPECT_EQ(describe_responses(*record), (Vector<ByteString> { "302" }));
    EXPECT(!request->done());
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_navigation_aborted_before_the_engine_runs_its_next_hop_completes_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto completions = make_ref_counted<ReachedSteps>();

    start_navigation(harness, completions, "http://127.0.0.1:8000/hop-taken-over"sv, "hop-taken-over"_utf16);
    auto redirect_id = request_server.wait_for_request("GET"sv);
    auto token = token_of_fetch_for("/hop-taken-over"sv);
    VERIFY(token.has_value());
    request_server.send_headers(redirect_id, 302, { location("/hop-taken-over-final"sv) });

    // The engine has been handed the follow of the redirect, which it runs one deferred invocation later.
    harness.run_until_step(commands, "follow redirect of 1"sv);
    change_ongoing_navigation(harness, "taking-over-hop"_utf16);
    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null" }));
    harness.run_until_step(commands, "abort 1"sv);
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(completions->steps, (Vector<ByteString> { "null" }));
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "follow redirect of 1", "end of fetch 1", "abort 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /hop-taken-over", "release lease 0", "stop 0" }));
    EXPECT(!is_rooted(*token));
}
