/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/BaseUriDirective.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(BaseUriDirective);

BaseUriDirective::BaseUriDirective(Utf16FlyString name, Vector<Utf16String> value)
    : Directive(move(name), move(value))
{
}

}
