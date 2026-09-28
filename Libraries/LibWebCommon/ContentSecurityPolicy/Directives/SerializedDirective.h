/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::ContentSecurityPolicy::Directives {

struct SerializedDirective {
    Utf16FlyString name;
    Vector<Utf16String> value;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ContentSecurityPolicy::Directives::SerializedDirective const&);

template<>
WEBCOMMON_API ErrorOr<Web::ContentSecurityPolicy::Directives::SerializedDirective> decode(Decoder&);

}
