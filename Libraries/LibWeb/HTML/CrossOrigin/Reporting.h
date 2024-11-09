/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#accessor-accessed-relationship
enum class AccessorAccessedRelationship {
    AccessorIsOpener,
    AccessorIsOpenee,
    None,
};

void check_if_access_between_two_browsing_contexts_should_be_reported(BrowsingContext const& accessor, BrowsingContext const* accessed, JS::PropertyKey const&, EnvironmentSettingsObject const&);

}
