/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
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
            .environment_id = {},
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
        .environment_id = {},
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
    m_response_body_request = request;

    auto weak_this = make_weak_ptr();
    request->set_unbuffered_request_callbacks(
        [weak_this](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CacheState) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(true);
        },
        [](Requests::ResponseData) {},
        [](Core::ImmutableBytes) {},
        [weak_this](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError>) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(false);
        });
}

void NavigationLoader::move_response_body_to(RequestServerInstance& request_server)
{
    if (!m_response_body_request || !m_response_body_request_server || m_response_body_request_server.ptr() == &request_server)
        return;

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle)
        return;

    auto exported = m_response_body_request_server->ui_client().export_request(*m_response_body_request);
    if (exported.is_error()) {
        warnln("Unable to move a navigation response between RequestServers: {}", exported.error());
        return;
    }

    auto request = request_server.ui_client().import_request(exported.release_value(), Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    request->set_body_delivery_paused(true);
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

// The window of the document the navigation params create takes over their reserved environment's id, which the UI
// process generates, and is in the agent cluster of the agent the UI process obtained for it. A navigation without a
// reserved environment gets one with the id.
void NavigationLoader::set_document(CanonicalDocument const& document, CanonicalNavigable const& navigable)
{
    VERIFY(m_result.has_value());
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
