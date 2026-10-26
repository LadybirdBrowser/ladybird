/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>

namespace Web::HTML {

struct SerializedPolicyContainer {
    Vector<ContentSecurityPolicy::SerializedPolicy> csp_list;
    EmbedderPolicy embedder_policy;
    ReferrerPolicy::ReferrerPolicy referrer_policy;
    bool force_load_at_top { false };
    UpgradeInsecureRequests::InsecureRequestsPolicy insecure_requests_policy { UpgradeInsecureRequests::InsecureRequestsPolicy::DoNotUpgrade };
    UpgradeInsecureRequests::UpgradeInsecureNavigationsSet upgrade_insecure_navigations_set {};
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SerializedPolicyContainer const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SerializedPolicyContainer> decode(Decoder&);

}
