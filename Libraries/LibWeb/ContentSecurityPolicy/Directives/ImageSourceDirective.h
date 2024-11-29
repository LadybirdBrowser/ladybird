/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-img-src
class ImageSourceDirective final : public Directive {
    GC_CELL(ImageSourceDirective, Directive)
    GC_DECLARE_ALLOCATOR(ImageSourceDirective);

public:
    virtual ~ImageSourceDirective() = default;

    [[nodiscard]] virtual Result pre_request_check(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Policy const>) const override;
    [[nodiscard]] virtual Result post_request_check(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<Policy const>) const override;

private:
    ImageSourceDirective(String name, Vector<String> value);
};

}
