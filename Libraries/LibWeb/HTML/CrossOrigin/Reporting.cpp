/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Ptr.h>
#include <LibWeb/HTML/CrossOrigin/Reporting.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#coop-check-access-report
void check_if_access_between_two_browsing_contexts_should_be_reported(
    GC::Ptr<BrowsingContext const>,
    GC::Ptr<BrowsingContext const>,
    JS::PropertyKey const&,
    GC::Ref<EnvironmentSettingsObject const>)
{
    // FIXME: Implement this.
}

}
