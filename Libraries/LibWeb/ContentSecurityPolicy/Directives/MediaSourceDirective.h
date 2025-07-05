/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-media-src
class MediaSourceDirective final : public Directive {
    GC_CELL(MediaSourceDirective, Directive)
    GC_DECLARE_ALLOCATOR(MediaSourceDirective);

public:
    virtual ~MediaSourceDirective() = default;

    virtual Result pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Policy const>) const override;
    virtual Result post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<Policy const>) const override;

private:
    MediaSourceDirective(String name, Vector<String> value);
};

}
