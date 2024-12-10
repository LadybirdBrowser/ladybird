/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-base-uri
class BaseUriDirective final : public Directive {
    GC_CELL(BaseUriDirective, Directive)
    GC_DECLARE_ALLOCATOR(BaseUriDirective);

public:
    virtual ~BaseUriDirective() = default;

private:
    BaseUriDirective(String name, Vector<String> value);
};

}
