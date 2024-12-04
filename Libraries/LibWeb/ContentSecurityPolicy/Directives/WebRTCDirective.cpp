/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/WebRTCDirective.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(WebRTCDirective);

WebRTCDirective::WebRTCDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#webrtc-pre-connect
Directive::Result WebRTCDirective::webrtc_pre_connect_check(GC::Ref<Policy const>) const
{
    // 1. If this directive’s value contains a single item which is an ASCII case-insensitive match for the string
    //    "'allow'", return "Allowed".
    if (value().size() == 1 && value().first().equals_ignoring_ascii_case("'allow'"sv))
        return Result::Allowed;

    // 2. Return "Blocked".
    return Result::Blocked;
}

}
