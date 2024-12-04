/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-report-to
class ReportToDirective final : public Directive {
    GC_CELL(ReportToDirective, Directive)
    GC_DECLARE_ALLOCATOR(ReportToDirective);

public:
    virtual ~ReportToDirective() = default;

private:
    ReportToDirective(String name, Vector<String> value);
};

}
