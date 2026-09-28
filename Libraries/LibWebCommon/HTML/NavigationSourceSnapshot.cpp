/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>

namespace Web::HTML {

NavigationSourceSnapshot create_navigation_source_snapshot_without_a_source_document()
{
    // 1. If sourceDocument is null, then return a new source snapshot params with
    return {
        // has transient activation
        //     true
        .has_transient_activation = true,
        // sandboxing flags
        //     an empty sandboxing flag set
        .sandboxing_flags = {},
        // allows downloading
        //     true
        .allows_downloading = true,
        // fetch client
        //     null
        .fetch_client = {},
        // source policy container
        //     a new policy container
        .source_policy_container = {
            .csp_list = {},
            .embedder_policy = {},
            .referrer_policy = ReferrerPolicy::DEFAULT_REFERRER_POLICY,
        },
    };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationSourceSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.has_transient_activation));
    TRY(encoder.encode(snapshot.sandboxing_flags));
    TRY(encoder.encode(snapshot.allows_downloading));
    TRY(encoder.encode(snapshot.fetch_client));
    TRY(encoder.encode(snapshot.source_policy_container));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationSourceSnapshot> decode(Decoder& decoder)
{
    return Web::HTML::NavigationSourceSnapshot {
        .has_transient_activation = TRY(decoder.decode<bool>()),
        .sandboxing_flags = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .allows_downloading = TRY(decoder.decode<bool>()),
        .fetch_client = TRY(decoder.decode<Optional<Web::HTML::SerializedEnvironmentSettingsObject>>()),
        .source_policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
    };
}

}
