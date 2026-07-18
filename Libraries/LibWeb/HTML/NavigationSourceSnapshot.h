/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// AD-HOC: When a cross-site navigation continues in a new WebContent process, the source document only exists in the
//         process where the navigation started. This carries the state that the navigate algorithm snapshots from the
//         source document, so the new process can continue the navigation as if it had snapshotted it locally.
struct NavigationSourceSnapshot {
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#source-snapshot-params
    // NB: The source snapshot params' fetch client cannot cross the process boundary; the receiving process
    //     substitutes its own environment as the request client, and the referrer below carries the value that fetch
    //     would have derived from the original fetch client.
    bool has_transient_activation { false };
    SandboxingFlagSet sandboxing_flags {};
    bool allows_downloading { true };
    SerializedPolicyContainer source_policy_container;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate, steps 3 and 4
    URL::Origin initiator_origin_snapshot;
    URL::URL initiator_base_url_snapshot;

    // AD-HOC: The referrer that fetch would resolve for the navigation request from the source document's fetch
    //         client, and the referrer policy the navigate algorithm was invoked with.
    URL::URL referrer;
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationSourceSnapshot const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationSourceSnapshot> decode(Decoder&);

}
