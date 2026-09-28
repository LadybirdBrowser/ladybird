/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Bindings/SecurityPolicyViolationEvent.h>
#include <LibWebCommon/ContentSecurityPolicy/Directives/SerializedDirective.h>
#include <LibWebCommon/ContentSecurityPolicy/PolicySource.h>
#include <LibWebCommon/Export.h>

namespace Web::ContentSecurityPolicy {

struct SerializedPolicy {
    Vector<Directives::SerializedDirective> directives;
    Bindings::SecurityPolicyViolationEventDisposition disposition;
    PolicySource source;
    URL::Origin self_origin;
    String pre_parsed_policy_string;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ContentSecurityPolicy::SerializedPolicy const&);

template<>
WEBCOMMON_API ErrorOr<Web::ContentSecurityPolicy::SerializedPolicy> decode(Decoder&);

}
