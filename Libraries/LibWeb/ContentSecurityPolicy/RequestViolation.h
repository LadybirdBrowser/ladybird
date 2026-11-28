/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/HTML/Scripting/EnvironmentSettingsObjectAddress.h>
#include <LibWeb/HTML/Scripting/RemoteClientNavigation.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::Fetch::Infrastructure {

class Request;

}

namespace Web::ContentSecurityPolicy {

// A violation of a policy for a request, with what reporting it needs from the request.
struct RequestViolation {
    struct LocalClient {
        HTML::EnvironmentSettingsObjectAddress address;
    };
    struct RemoteClient {
        // The policy's index in the request's CSP list.
        u64 policy_index { 0 };
        Optional<HTML::RemoteClientNavigation> navigation;
    };
    // The container document of the child navigable the request navigates.
    struct ContainerDocument {
        HTML::CrossProcessId navigable_id;
        Optional<Utf16String> navigation_id;
        // The policy's index in the container document's CSP list.
        u64 policy_index { 0 };
    };
    using ReportedTo = Variant<LocalClient, RemoteClient, ContainerDocument>;

    NonnullRefPtr<Policy const> policy;
    Utf16FlyString effective_directive;
    // The request's URL, not its current URL.
    URL::URL resource;
    ReportedTo reported_to;
};

[[nodiscard]] RequestViolation violation_of_request(Fetch::Infrastructure::Request const&, NonnullRefPtr<Policy const>);

// The policy's index in the request's CSP list, which is a copy of its client's.
[[nodiscard]] u64 index_of_policy_in_requests_csp_list(Fetch::Infrastructure::Request const&, Policy const&);

}
