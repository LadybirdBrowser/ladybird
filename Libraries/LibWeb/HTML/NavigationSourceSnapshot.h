/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>

namespace Web::HTML {

// The process-safe representation of source snapshot params used by the UI-owned navigation transaction.
struct NavigationSourceSnapshot {
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#source-snapshot-params
    bool has_transient_activation { false };
    SandboxingFlagSet sandboxing_flags {};
    bool allows_downloading { true };
    Optional<SerializedEnvironmentSettingsObject> fetch_client;
    SerializedPolicyContainer source_policy_container;
};

WEB_API NavigationSourceSnapshot create_navigation_source_snapshot(SourceSnapshotParams const&);

WEB_API GC::Ref<SourceSnapshotParams> create_source_snapshot_params_from_navigation_source_snapshot(JS::Realm&, NavigationSourceSnapshot const&);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationSourceSnapshot const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationSourceSnapshot> decode(Decoder&);

}
