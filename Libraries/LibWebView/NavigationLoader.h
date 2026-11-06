/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibHTTP/Forward.h>
#include <LibRequests/Forward.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestTransferLease.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/RequestServerManager.h>

namespace WebView {

// UI-process owner of navigation population state. It keeps the pending entry
// and response body alive while the document host is being selected.
class WEBVIEW_API NavigationLoader final
    : public RefCounted<NavigationLoader>
    , public Weakable<NavigationLoader> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullRefPtr<NavigationLoader> create(Web::HTML::NavigationPopulationRequest request)
    {
        return adopt_ref(*new NavigationLoader(move(request)));
    }

    ~NavigationLoader();

    struct ResponseDocument {
        // Created for inline content that doesn't have a DOM: the error page for a failed navigation.
        bool is_inline_content { false };
        Web::HTML::OpenerPolicyEnforcementResult coop_enforcement_result;
        URL::URL response_url;
        Optional<URL::URL> request_current_url;
        URL::Origin origin;
        Web::HTML::OpenerPolicy opener_policy;
        // The embedder policy of navigationParams's policy container.
        Web::HTML::EmbedderPolicy embedder_policy;
        Web::HTML::SandboxingFlagSet final_sandboxing_flag_set {};
        // The id of the window environment a process created the document with before the UI process heard of it.
        Optional<Web::HTML::EnvironmentId> environment_id;
        Web::HTML::CrossProcessId document_id;
        // The permissions policy created for navigationParams in the UI process, if it handled them.
        Optional<Web::PermissionsPolicy::PermissionsPolicy> permissions_policy {};
        bool requests_oac { false };
    };
    Optional<ResponseDocument> response_document() const;
    void set_document(CanonicalDocument const&, CanonicalNavigable const&);

    void refuse_made_up_local_file_urls(WebContentClient const& population_worker, Web::HTML::NavigationPopulationResult&) const;
    void did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult);
    void acquire_response_body(WebContentClient const& population_worker, Function<void(bool)> completion_steps);
    void obtain_the_response_policies(CanonicalNavigable const&);
    void check_the_responses_adherence_to_its_embedder_policy(CanonicalNavigable const&);
    // Moves the response to the document host's RequestServer for its top-level site, if different.
    void move_response_body_to(RequestServerSiteBindings const& host_bindings, Function<void(bool)> completion_steps);
    bool response_body_matches(int request_server_client_id, u64 request_server_request_id) const;
    Optional<Requests::RequestTransferLeaseKey> const& response_body_transfer_lease() const { return m_response_body_transfer_lease; }
    // The host may adopt the response body once. The steps run once RequestServer has answered, or at once if there is
    // nothing to designate.
    void let_host_adopt_response_body(WebContentPage const& host, Function<void()> steps);
    bool response_body_was_handed_to_another_process(WebContentClient const&) const;
    // The host's client that RequestServer was told may adopt the response body.
    Optional<int> response_body_adopter_client_id() const { return m_response_body_adopter_client_id; }
    Web::HTML::NavigationPopulationRequest const& request() const { return m_request; }
    Web::HTML::NavigationPopulationResult const& result() const;
    Web::HTML::NavigationPopulationResult take_result();
    void reclaim_response_body_after_failed_handoff();

    static bool response_body_belongs_to_another_process(Web::HTML::NavigationPopulationResult const&, WebContentClient const&);
    static void discard(WebContentClient const& population_worker, Web::HTML::NavigationPopulationResult&);

private:
    explicit NavigationLoader(Web::HTML::NavigationPopulationRequest request)
        : m_request(move(request))
    {
    }

    void determine_the_origin_of_the_response();
    void did_acquire(bool succeeded);
    void set_response_body_callbacks(Requests::Request&);
    void did_designate_response_body_adopter();
    void release_response_body();

    Web::HTML::NavigationPopulationRequest m_request;
    Optional<Web::HTML::NavigationPopulationResult> m_result;
    RefPtr<Requests::Request> m_response_body_request;
    RefPtr<HTTP::HeaderList> m_response_header_list;
    RefPtr<RequestServerInstance> m_response_body_request_server;
    Optional<Requests::RequestTransferLeaseKey> m_response_body_transfer_lease;
    // The top-level site of the document the response creates.
    Optional<Utf16String> m_top_level_site;
    bool m_response_body_was_handed_off { false };
    WeakPtr<WebContentClient> m_response_body_host;
    Optional<int> m_response_body_adopter_client_id;
    Function<void(bool)> m_completion_steps;
    Function<void()> m_steps_after_adopter_designation;
};

}
