/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

struct SerializedDirective {
    String name;
    Vector<String> value;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::ContentSecurityPolicy::Directives::SerializedDirective const&);

template<>
ErrorOr<Web::ContentSecurityPolicy::Directives::SerializedDirective> decode(Decoder&);

}
