/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/TargetSnapshotParams.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-target-snapshot-params
TargetSnapshotParams snapshot_target_snapshot_params(LocalNavigable& target_navigable)
{
    // To snapshot target snapshot params given a navigable targetNavigable, return a new target snapshot params with:
    return {
        // sandboxing flags
        //     the result of determining the creation sandboxing flags given targetNavigable's active browsing
        //     context and targetNavigable's container
        .sandboxing_flags = determine_the_creation_sandboxing_flags(*target_navigable.active_browsing_context(), target_navigable.container()),

        // iframe element referrer policy
        //     the result of determining the iframe element referrer policy given targetNavigable's container
        .iframe_element_referrer_policy = determine_iframe_element_referrer_policy(target_navigable.container()),
    };
}

}
