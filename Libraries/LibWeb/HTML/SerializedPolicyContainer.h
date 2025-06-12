/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

struct SerializedPolicyContainer {
    Vector<ContentSecurityPolicy::SerializedPolicy> csp_list;
    EmbedderPolicy embedder_policy;
    ReferrerPolicy::ReferrerPolicy referrer_policy;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SerializedPolicyContainer const&);

template<>
ErrorOr<Web::HTML::SerializedPolicyContainer> decode(Decoder&);

}
