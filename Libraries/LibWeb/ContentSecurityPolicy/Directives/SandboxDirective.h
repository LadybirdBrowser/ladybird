/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-sandbox
class SandboxDirective final : public Directive {
    GC_CELL(SandboxDirective, Directive)
    GC_DECLARE_ALLOCATOR(SandboxDirective);

public:
    virtual ~SandboxDirective() = default;

    [[nodiscard]] virtual Result initialization(Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>>, GC::Ref<Policy const>) const override;

private:
    SandboxDirective(String name, Vector<String> value);
};

}
