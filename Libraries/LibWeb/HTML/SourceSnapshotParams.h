/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#source-snapshot-params
struct SourceSnapshotParams {
    // a boolean
    bool has_transient_activation;

    // a sandboxing flag set
    SandboxingFlagSet sandboxing_flags = {};

    // a boolean
    bool allows_downloading;

    // an environment settings object, only to be used as a request client
    JS::GCPtr<EnvironmentSettingsObject> fetch_client;

    // a policy container
    PolicyContainer source_policy_container;
};

}
