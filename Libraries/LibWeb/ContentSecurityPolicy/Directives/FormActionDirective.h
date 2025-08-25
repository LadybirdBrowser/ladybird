/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-form-action
class FormActionDirective final : public Directive {
    GC_CELL(FormActionDirective, Directive)
    GC_DECLARE_ALLOCATOR(FormActionDirective);

public:
    virtual ~FormActionDirective() = default;

    virtual Result pre_navigation_check(GC::Ref<Fetch::Infrastructure::Request>, NavigationType, GC::Ref<Policy const>) const override;

private:
    FormActionDirective(String name, Vector<String> value);
};

}
