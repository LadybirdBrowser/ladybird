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
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWebView/PrivateBrowsing.h>

namespace WebView {

// UI-process owner of navigation population state. It keeps the pending entry
// and response body alive while the document host is being selected.
class NavigationLoader final : public Weakable<NavigationLoader> {
public:
    static NonnullOwnPtr<NavigationLoader> create(IsPrivate is_private, Web::HTML::NavigationPopulationRequest request)
    {
        return adopt_own(*new NavigationLoader(is_private, move(request)));
    }

    ~NavigationLoader();

    struct ResponseDocument {
        Web::HTML::OpenerPolicyEnforcementResult coop_enforcement_result;
        URL::URL url;
        URL::Origin origin;
    };
    Optional<ResponseDocument> response_document() const;

    void did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult);
    void acquire_response_body(Function<void(bool)> completion_steps);
    bool response_body_matches(int request_server_client_id, u64 request_server_request_id) const;
    Web::HTML::NavigationPopulationRequest const& request() const { return m_request; }
    Web::HTML::NavigationPopulationResult const& result() const;
    Web::HTML::NavigationPopulationResult take_result();
    void reclaim_response_body_after_failed_handoff();

    static void discard(IsPrivate, Web::HTML::NavigationPopulationResult&);

private:
    NavigationLoader(IsPrivate is_private, Web::HTML::NavigationPopulationRequest request)
        : m_is_private(is_private)
        , m_request(move(request))
    {
    }

    void did_acquire(bool succeeded);
    void release_response_body();

    IsPrivate m_is_private { IsPrivate::No };
    Web::HTML::NavigationPopulationRequest m_request;
    Optional<Web::HTML::NavigationPopulationResult> m_result;
    RefPtr<Requests::Request> m_response_body_request;
    Optional<int> m_response_body_request_server_client_id;
    Optional<u64> m_response_body_request_server_request_id;
    bool m_response_body_was_handed_off { false };
    Function<void(bool)> m_completion_steps;
};

}
