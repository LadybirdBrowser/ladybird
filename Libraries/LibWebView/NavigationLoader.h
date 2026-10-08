/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <LibRequests/Forward.h>
#include <LibRequests/Request.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/RequestServerManager.h>

namespace WebView {

// UI-process owner of navigation population state. It keeps the pending entry
// and response body alive while the document host is being selected.
class WEBVIEW_API NavigationLoader final : public Weakable<NavigationLoader> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<NavigationLoader> create(IsPrivate, Web::HTML::NavigationPopulationRequest request)
    {
        return adopt_own(*new NavigationLoader(move(request)));
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
        // The id of the window environment a process created the document with before the UI process heard of it.
        Optional<Web::HTML::EnvironmentId> environment_id;
    };
    Optional<ResponseDocument> response_document() const;
    void set_document(CanonicalDocument const&, CanonicalNavigable const&);

    void did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult);
    void acquire_response_body(Function<void(bool)> completion_steps);
    // Moves the response over to the RequestServer of the process that will host the document, if that is another one.
    void move_response_body_to(RequestServerInstance&);
    bool response_body_matches(int request_server_client_id, u64 request_server_request_id) const;
    Web::HTML::NavigationPopulationRequest const& request() const { return m_request; }
    Web::HTML::NavigationPopulationResult const& result() const;
    Web::HTML::NavigationPopulationResult take_result();
    void reclaim_response_body_after_failed_handoff();

    static void discard(IsPrivate, Web::HTML::NavigationPopulationResult&);

private:
    explicit NavigationLoader(Web::HTML::NavigationPopulationRequest request)
        : m_request(move(request))
    {
    }

    void determine_the_origin_of_the_response();
    void did_acquire(bool succeeded);
    void release_response_body();

    Web::HTML::NavigationPopulationRequest m_request;
    Optional<Web::HTML::NavigationPopulationResult> m_result;
    RefPtr<Requests::Request> m_response_body_request;
    RefPtr<RequestServerInstance> m_response_body_request_server;
    Optional<int> m_response_body_request_server_client_id;
    Optional<u64> m_response_body_request_server_request_id;
    bool m_response_body_was_handed_off { false };
    Function<void(bool)> m_completion_steps;
};

}
