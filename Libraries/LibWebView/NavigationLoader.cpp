/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicyEnforcement.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/RequestServerManager.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
// The document that the task queued by step 5 creates, if it creates one.
Optional<NavigationLoader::ResponseDocument> NavigationLoader::response_document() const
{
    auto const& navigation_params = result().navigation_params;

    // 3. If navigationParams is a non-fetch scheme navigation params:
    //    1. Set entry's document state's document to the result of running attempt to create a non-fetch scheme
    //       document given navigationParams.
    // NB: That hands the URL off to external software, which creates no document.
    if (navigation_params.has<Web::HTML::NonFetchSchemeNavigationParamsDescriptor>())
        return {};

    // 4. Otherwise, if any of the following are true:
    //    - navigationParams is null;
    //    [...]
    //    then:
    //    1. Set entry's document state's document to the result of creating a document for inline content that
    //       doesn't have a DOM, given navigable, null, navTimingType, and userInvolvement. The inline content
    //       should indicate to the user the sort of error that occurred.
    // NB: That document is created with the new opaque origin the result carries to the process creating it, a new
    //     opener policy enforcement result, and about:error as its URL.
    if (navigation_params.has<Web::HTML::NavigationParamsNullOrError>()) {
        auto origin = *result().inline_content_origin;
        return ResponseDocument {
            .is_inline_content = true,
            .coop_enforcement_result = { .url = URL::about_error(), .origin = origin, .opener_policy = {} },
            .response_url = URL::about_error(),
            .request_current_url = {},
            .origin = origin,
            .opener_policy = {},
            .embedder_policy = {},
            .final_sandboxing_flag_set = {},
            .environment_id = {},
            .document_id = {},
        };
    }

    auto const& fetched_navigation_params = navigation_params.get<Web::HTML::NavigationParamsDescriptor>();

    // FIXME: 5. Otherwise, if navigationParams's response has a `Content-Disposition` header specifying the
    //           attachment disposition type: [...]
    //        A download creates no document.

    // 6. Otherwise, if navigationParams's response's status is not 204 and is not 205, then set entry's
    //    document state's document to the result of loading a document given navigationParams,
    //    sourceSnapshotParams, and entry's document state's initiator origin.
    if (fetched_navigation_params.response.status == 204 || fetched_navigation_params.response.status == 205)
        return {};
    return ResponseDocument {
        .is_inline_content = false,
        .coop_enforcement_result = fetched_navigation_params.coop_enforcement_result,
        // The COOP enforcement result still describes the source document until an opener policy is enforced.
        // Placement uses the response's final URL, including any redirects.
        .response_url = fetched_navigation_params.response.url_list.is_empty() ? m_request.history_entry.url : fetched_navigation_params.response.url_list.last(),
        .request_current_url = fetched_navigation_params.request.has_value() && !fetched_navigation_params.request->url_list.is_empty()
            ? Optional<URL::URL> { fetched_navigation_params.request->url_list.last() }
            : Optional<URL::URL> {},
        .origin = fetched_navigation_params.origin,
        .opener_policy = fetched_navigation_params.opener_policy,
        .embedder_policy = fetched_navigation_params.policy_container.embedder_policy,
        .final_sandboxing_flag_set = fetched_navigation_params.final_sandboxing_flag_set,
        .environment_id = {},
        .document_id = {},
    };
}

static Web::HTML::NavigationResponseBodyHandle* response_body_handle(Web::HTML::NavigationPopulationResult& result)
{
    if (!result.navigation_params.has<Web::HTML::NavigationParamsDescriptor>())
        return nullptr;
    auto& response = result.navigation_params.get<Web::HTML::NavigationParamsDescriptor>().response;
    return response.body.get_pointer<Web::HTML::NavigationResponseBodyHandle>();
}

NavigationLoader::~NavigationLoader()
{
    release_response_body();
}

void NavigationLoader::did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult result)
{
    VERIFY(!m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    // NB: The process that fetched the response reports the URLs it reached. Fetching never redirects a request to a
    //     file: URL, so one among them on a navigation the UI process did not admit to a file: URL is made up: the
    //     document it would create could read local files. Such a navigation creates an error document instead.
    if (m_request.history_entry.url.scheme() != "file"sv) {
        auto is_file_url = [](URL::URL const& url) { return url.scheme() == "file"sv; };
        auto names_a_local_file = result.redirected_url.has_value() && is_file_url(*result.redirected_url);
        if (auto const* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>()) {
            names_a_local_file |= any_of(navigation_params->response.url_list, is_file_url);
            if (navigation_params->request.has_value())
                names_a_local_file |= any_of(navigation_params->request->url_list, is_file_url);
        }
        if (names_a_local_file) {
            discard(m_is_private, result);
            result.navigation_params = Web::HTML::NavigationParamsNullOrError {};
            result.redirected_url.clear();
        }
    }

    Web::HTML::apply_navigation_population_result(m_request, result);
    m_result = move(result);
    m_result->inline_content_origin = URL::Origin::create_opaque();
    determine_the_origin_of_the_response();
}

// NB: The process that created the navigation params determined responseOrigin for its own checks. The UI process
//     runs the step again with what it holds, and the process creating the document takes the origin from here.
void NavigationLoader::determine_the_origin_of_the_response()
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    auto const& entry = m_request.history_entry;
    auto const& target_snapshot_params = m_request.target_snapshot_params;
    auto response_url = navigation_params->response.url_list.is_empty() ? entry.url : navigation_params->response.url_list.last();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-from-a-srcdoc-resource
    if (Web::HTML::url_matches_about_srcdoc(response_url)) {
        // 3. Let responseOrigin be the result of determining the origin given response's URL, targetSnapshotParams's
        //    sandboxing flags, and entry's document state's origin.
        // AD-HOC: Determining the origin asserts that sourceOrigin is non-null for about:srcdoc, which the process that
        //         gave entry need not uphold. Such a document gets a new opaque origin.
        if (!entry.document_state.origin.has_value()) {
            navigation_params->origin = URL::Origin::create_opaque();
            return;
        }
        navigation_params->origin = Web::HTML::determine_the_origin(response_url, target_snapshot_params.sandboxing_flags, entry.document_state.origin);
        return;
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
    // 10. Set finalSandboxFlags to the union of targetSnapshotParams's sandboxing flags and responsePolicyContainer's CSP
    //     list's CSP-derived sandboxing flags.
    // NB: The process that fetched the response created responsePolicyContainer, and its finalSandboxFlags hold the
    //     CSP-derived sandboxing flags. Sandboxing flags only ever make an origin opaque, so the UI process takes them.
    navigation_params->final_sandboxing_flag_set |= target_snapshot_params.sandboxing_flags;

    // 11. Set responseOrigin to the result of determining the origin given response's URL, finalSandboxFlags, and
    //     entry's document state's initiator origin.
    navigation_params->origin = Web::HTML::determine_the_origin(response_url, navigation_params->final_sandboxing_flag_set, entry.document_state.initiator_origin);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
// NB: The process that fetched the response can host another site, so the UI process obtains the response's opener and
//     embedder policies itself, from the headers that RequestServer gave it with the response.
void NavigationLoader::obtain_the_response_policies(CanonicalNavigable const& navigable)
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    auto const& response_url = navigation_params->response.url_list.is_empty() ? m_request.history_entry.url : navigation_params->response.url_list.last();

    // NB: A response that the fetching process read from its own HTTP memory cache has no RequestServer request, so
    //     its headers are the ones that process reported.
    auto header_list = m_response_header_list ? NonnullRefPtr { *m_response_header_list } : HTTP::HeaderList::create(navigation_params->response.headers);

    // 17. Let responseCOOP be a new opener policy.
    Web::HTML::OpenerPolicy response_coop;

    // 21. While true:
    //     2. If request's reserved client is null:
    //        1. Let topLevelCreationURL be currentURL.
    //        3. If navigable is not a top-level traversable:
    //           1. Let parentEnvironment be navigable's parent's active document's relevant settings object.
    //           2. Set topLevelCreationURL to parentEnvironment's top-level creation URL.
    // NB: The UI process runs these steps for the final response only. The parent environment's top-level creation URL
    //     is the creation URL of the top-level traversable's active document.
    auto const& top_level_creation_url = navigable.is_top_level_traversable() ? response_url : navigable.top_level_traversable().active_document().creation_url();

    // https://html.spec.whatwg.org/multipage/webappapis.html#secure-context
    // 2. If environment's top-level creation URL's scheme is "data", then return false.
    // 3. If the result of Is url potentially trustworthy? given environment's top-level creation URL is "Potentially
    //    Trustworthy", then return true.
    auto reserved_environment_is_a_non_secure_context = top_level_creation_url.scheme() == "data"sv
        || Web::SecureContexts::is_url_potentially_trustworthy(top_level_creation_url) == Web::SecureContexts::Trustworthiness::NotTrustworthy;

    //     9. Set responsePolicyContainer to the result of creating a policy container from a fetch response given
    //        response and request's reserved client.
    // NB: The document takes responsePolicyContainer unless the response's URL is local, when it inherits a policy
    //     container instead.
    if (!Web::Fetch::Infrastructure::is_local_url(response_url))
        navigation_params->policy_container.embedder_policy = Web::HTML::obtain_an_embedder_policy(header_list, reserved_environment_is_a_non_secure_context);

    //     12. If navigable is a top-level traversable:
    if (navigable.is_top_level_traversable()) {
        // 1. Set responseCOOP to the result of obtaining an opener policy given response and request's reserved client.
        response_coop = Web::HTML::obtain_an_opener_policy(header_list, reserved_environment_is_a_non_secure_context);

        // 2. Set coopEnforcementResult to the result of enforcing the response's opener policy given navigable's active
        //    browsing context, response's URL, responseOrigin, responseCOOP, coopEnforcementResult, and request's
        //    referrer.
        // NB: The fetching process enforced the opener policy of each response along the redirects, which the UI
        //     process does not see. Enforcing the final response's opener policy again, against navigable's active
        //     document, lets that process add a browsing context group switch but not withhold one. Only the switch
        //     flags are taken from this result.
        auto const& active_document = navigable.active_document();
        Web::HTML::OpenerPolicyEnforcementResult active_document_coop_enforcement_result {
            .needs_a_browsing_context_group_switch = false,
            .would_need_a_browsing_context_group_switch_due_to_report_only = false,
            .url = active_document.creation_url(),
            .origin = active_document.origin(),
            .opener_policy = active_document.opener_policy(),
            .current_context_is_navigation_source = false,
        };
        auto coop_enforcement_result = Web::HTML::enforce_a_responses_opener_policy(active_document.is_initial_about_blank(), response_url, navigation_params->origin, response_coop, active_document_coop_enforcement_result);
        navigation_params->coop_enforcement_result.opener_policy = response_coop;
        navigation_params->coop_enforcement_result.needs_a_browsing_context_group_switch |= coop_enforcement_result.needs_a_browsing_context_group_switch;
        navigation_params->coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only |= coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only;
    }

    navigation_params->opener_policy = response_coop;
}

// https://html.spec.whatwg.org/multipage/browsers.html#check-a-navigation-response's-adherence-to-its-embedder-policy
// NB: response is only used for violation reports, which are not queued.
static bool check_a_navigation_responses_adherence_to_its_embedder_policy(CanonicalNavigable const& navigable, Web::HTML::EmbedderPolicy const& response_policy)
{
    // 1. If navigable is not a child navigable, then return true.
    auto const* container_document = navigable.container_document();
    if (!container_document)
        return true;

    // 2. Let parentPolicy be navigable's container document's policy container's embedder policy.
    auto const& parent_policy = container_document->embedder_policy();

    // FIXME: 3. If parentPolicy's report-only value is compatible with cross-origin isolation and responsePolicy's value
    //           is not, then queue a cross-origin embedder policy inheritance violation with response, "navigation",
    //           parentPolicy's report-only reporting endpoint, "reporting", and navigable's container document's
    //           relevant settings object.

    // 4. If parentPolicy's value is not compatible with cross-origin isolation or responsePolicy's value is compatible
    //    with cross-origin isolation, then return true.
    if (!Web::HTML::is_compatible_with_cross_origin_isolation(parent_policy.value) || Web::HTML::is_compatible_with_cross_origin_isolation(response_policy.value))
        return true;

    // FIXME: 5. Queue a cross-origin embedder policy inheritance violation with response, "navigation", parentPolicy's
    //           reporting endpoint, "enforce", and navigable's container document's relevant settings object.

    // 6. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
// NB: The UI process checks the response's adherence to its embedder policy, as it holds navigable's container
//     document.
void NavigationLoader::check_the_responses_adherence_to_its_embedder_policy(CanonicalNavigable const& navigable)
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    // 5. Queue a global task on the navigation and traversal task source, given navigable's active window, to run these
    //    steps:
    //    4. Otherwise, if any of the following are true:
    //       [...]
    //       - navigationParams's reserved environment is non-null and the result of checking a navigation response's
    //         adherence to its embedder policy given navigationParams's response, navigable, and navigationParams's
    //         policy container's embedder policy is false; or
    //       [...]
    //       then:
    //       1. Set entry's document state's document to the result of creating a document for inline content that
    //          doesn't have a DOM, given navigable, null, navTimingType, and userInvolvement. The inline content
    //          should indicate to the user the sort of error that occurred.
    // NB: The UI process checks this before the page that hosts the document queues the task, which creates that
    //     document for navigation params that are null.
    if (!navigation_params->reserved_environment.has_value()
        || check_a_navigation_responses_adherence_to_its_embedder_policy(navigable, navigation_params->policy_container.embedder_policy)) {
        return;
    }
    release_response_body();
    m_result->navigation_params = Web::HTML::NavigationParamsNullOrError { "Blocked by Cross-Origin-Embedder-Policy"_utf16 };
}

void NavigationLoader::acquire_response_body(Function<void(bool)> completion_steps)
{
    VERIFY(m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    m_completion_steps = move(completion_steps);

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle) {
        did_acquire(true);
        return;
    }

    // The response lives in the RequestServer of the client that fetched it.
    auto request_server = RequestServerManager::the().instance_for_client(body_handle->request_server_client_id);
    if (!request_server) {
        did_acquire(false);
        return;
    }
    auto request = request_server->ui_client().adopt_request(
        body_handle->request_server_client_id,
        body_handle->request_server_request_id,
        Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    m_response_body_request_server = request_server;
    m_response_body_request_server_client_id = body_handle->request_server_client_id;
    m_response_body_request_server_request_id = body_handle->request_server_request_id;

    request->set_body_delivery_paused(true);
    set_response_body_callbacks(*request);
    m_response_body_request = request;
}

void NavigationLoader::set_response_body_callbacks(Requests::Request& request)
{
    auto weak_this = make_weak_ptr();
    request.set_unbuffered_request_callbacks(
        [weak_this](NonnullRefPtr<HTTP::HeaderList> response_header_list, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CacheState) {
            if (auto* loader = weak_this.ptr()) {
                loader->m_response_header_list = move(response_header_list);
                loader->did_acquire(true);
            }
        },
        [](Requests::ResponseData) { },
        [](Core::ImmutableBytes) { },
        [weak_this](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError>) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(false);
        });
}

void NavigationLoader::move_response_body_to(RequestServerInstance& request_server, Function<void(bool)> completion_steps)
{
    VERIFY(!m_completion_steps);
    m_completion_steps = move(completion_steps);

    if (request_server.is_retired() || (m_response_body_request_server && m_response_body_request_server->is_retired())) {
        did_acquire(false);
        return;
    }
    if (!m_response_body_request || !m_response_body_request_server || m_response_body_request_server.ptr() == &request_server) {
        did_acquire(true);
        return;
    }

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle) {
        did_acquire(true);
        return;
    }

    auto exported = m_response_body_request_server->ui_client().export_request(*m_response_body_request);
    if (exported.is_error()) {
        warnln("Unable to move a navigation response between RequestServers: {}", exported.error());
        did_acquire(false);
        return;
    }

    auto request = request_server.ui_client().import_request(exported.release_value(), Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    request->set_body_delivery_paused(true);
    set_response_body_callbacks(*request);
    m_response_body_request = request;
    m_response_body_request_server = request_server;

    // The handle names the lease the host adopts, which is now the UI process's request in the host's RequestServer.
    body_handle->request_server_client_id = request_server.ui_client().request_server_client_id();
    body_handle->request_server_request_id = request->id();
    m_response_body_request_server_client_id = body_handle->request_server_client_id;
    m_response_body_request_server_request_id = body_handle->request_server_request_id;
}

Web::HTML::NavigationPopulationResult NavigationLoader::take_result()
{
    VERIFY(m_result.has_value());
    if (m_response_body_request)
        m_response_body_was_handed_off = true;
    return m_result.release_value();
}

bool NavigationLoader::response_body_matches(int request_server_client_id, u64 request_server_request_id) const
{
    return m_response_body_request_server_client_id == request_server_client_id
        && m_response_body_request_server_request_id == request_server_request_id;
}

void NavigationLoader::reclaim_response_body_after_failed_handoff()
{
    if (!m_response_body_was_handed_off)
        return;
    m_response_body_was_handed_off = false;
    release_response_body();
}

Web::HTML::NavigationPopulationResult const& NavigationLoader::result() const
{
    VERIFY(m_result.has_value());
    return *m_result;
}

// The document the result creates takes the UI process's id for it. Its window takes over the navigation params'
// reserved environment's id, which the UI process generates, and is in the agent cluster of the agent the UI process
// obtained for it. A navigation without a reserved environment gets one with the id.
void NavigationLoader::set_document(CanonicalDocument const& document, CanonicalNavigable const& navigable)
{
    VERIFY(m_result.has_value());
    m_result->document_id = document.id();

    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;
    auto const& window = document.relevant_global_object();
    if (!navigation_params->reserved_environment.has_value()) {
        navigation_params->reserved_environment = Web::HTML::NavigationEnvironmentDescriptor {
            .id = {},
            .creation_url = m_request.history_entry.url,
            .top_level_creation_url = {},
            .top_level_origin = {},
        };
    }
    navigation_params->reserved_environment->id = window.relevant_settings_object().id();
    navigation_params->agent_cluster_id = window.agent().agent_cluster_id();
    navigation_params->agent_cluster_cross_origin_isolation_mode = window.agent().agent_cluster_cross_origin_isolation_mode();

    if (&document.browsing_context() != &navigable.active_browsing_context())
        navigation_params->new_browsing_context_group_id = document.browsing_context().group()->id();
}

void NavigationLoader::discard(IsPrivate, Web::HTML::NavigationPopulationResult& result)
{
    auto* body_handle = response_body_handle(result);
    if (!body_handle)
        return;

    auto request_server = RequestServerManager::the().instance_for_client(body_handle->request_server_client_id);
    if (!request_server)
        return;
    auto request = request_server->ui_client().adopt_request(body_handle->request_server_client_id, body_handle->request_server_request_id);
    if (request)
        request->stop();
}

void NavigationLoader::did_acquire(bool succeeded)
{
    if (!m_completion_steps)
        return;

    auto completion_steps = move(m_completion_steps);
    completion_steps(succeeded);
}

void NavigationLoader::release_response_body()
{
    if (!m_response_body_request)
        return;

    // Once the descriptor has been sent, the destination is responsible for adopting the request. Dropping our
    // local reference before that IPC is handled must not release the lease or cancel the request being transferred.
    if (m_response_body_was_handed_off) {
        m_response_body_request = nullptr;
        return;
    }

    m_response_body_request->release_transfer_lease();
    m_response_body_request->stop();
    m_response_body_request = nullptr;
}

}
