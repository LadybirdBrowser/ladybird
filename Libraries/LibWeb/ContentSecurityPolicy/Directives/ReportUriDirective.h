/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-report-uri
class ReportUriDirective final : public Directive {
    GC_CELL(ReportUriDirective, Directive)
    GC_DECLARE_ALLOCATOR(ReportUriDirective);

public:
    virtual ~ReportUriDirective() = default;

private:
    ReportUriDirective(String name, Vector<String> value);
};

}
