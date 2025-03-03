/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SerializedDirective.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>

namespace Web::ContentSecurityPolicy {

struct SerializedPolicy {
    Vector<Directives::SerializedDirective> directives;
    Policy::Disposition disposition;
    Policy::Source source;
    URL::Origin self_origin;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::ContentSecurityPolicy::SerializedPolicy const&);

template<>
ErrorOr<Web::ContentSecurityPolicy::SerializedPolicy> decode(Decoder&);

}
