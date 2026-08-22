/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#target-snapshot-params
struct TargetSnapshotParams {
    // sandboxing flags: a sandboxing flag set
    SandboxingFlagSet sandboxing_flags {};

    // iframe element referrer policy: a referrer policy
    ReferrerPolicy::ReferrerPolicy iframe_element_referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
};

TargetSnapshotParams snapshot_target_snapshot_params(LocalNavigable& target_navigable);

}
