/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>

namespace Web::HTML {

NavigationSourceSnapshot create_navigation_source_snapshot(SourceSnapshotParams const& snapshot)
{
    return {
        .has_transient_activation = snapshot.has_transient_activation,
        .sandboxing_flags = snapshot.sandboxing_flags,
        .allows_downloading = snapshot.allows_downloading,
        .fetch_client = snapshot.fetch_client ? Optional<SerializedEnvironmentSettingsObject> { snapshot.fetch_client->serialize() } : Optional<SerializedEnvironmentSettingsObject> {},
        .source_policy_container = snapshot.source_policy_container->serialize(),
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
