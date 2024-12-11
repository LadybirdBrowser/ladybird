/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-webrtc
class WebRTCDirective final : public Directive {
    GC_CELL(WebRTCDirective, Directive)
    GC_DECLARE_ALLOCATOR(WebRTCDirective);

public:
    virtual ~WebRTCDirective() = default;

    [[nodiscard]] virtual Result webrtc_pre_connect_check(GC::Ref<Policy const>) const override;

private:
    WebRTCDirective(String name, Vector<String> value);
};

}
