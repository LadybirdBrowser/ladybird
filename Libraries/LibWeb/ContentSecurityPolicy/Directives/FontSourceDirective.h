/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-font-src
class FontSourceDirective final : public Directive {
    GC_CELL(FontSourceDirective, Directive)
    GC_DECLARE_ALLOCATOR(FontSourceDirective);

public:
    virtual ~FontSourceDirective() = default;

    [[nodiscard]] virtual Result pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Policy const>) const override;
    [[nodiscard]] virtual Result post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<Policy const>) const override;

private:
    FontSourceDirective(String name, Vector<String> value);
};

}
