/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>

namespace Web::HTML::CustomElementReactionNames {

#define __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_CUSTOM_ELEMENT_REACTION_NAMES
#undef __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME

}
