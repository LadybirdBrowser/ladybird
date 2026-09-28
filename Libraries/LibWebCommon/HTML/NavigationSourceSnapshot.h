/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HTML/SerializedPolicyContainer.h>

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

WEBCOMMON_API NavigationSourceSnapshot create_navigation_source_snapshot_without_a_source_document();

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationSourceSnapshot const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::NavigationSourceSnapshot> decode(Decoder&);

}
