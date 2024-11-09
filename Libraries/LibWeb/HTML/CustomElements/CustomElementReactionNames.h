/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::HTML::CustomElementReactionNames {

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-lifecycle-callbacks
#define ENUMERATE_CUSTOM_ELEMENT_REACTION_NAMES                        \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(connectedCallback)        \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(disconnectedCallback)     \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(adoptedCallback)          \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(attributeChangedCallback) \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(formAssociatedCallback)   \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(formDisabledCallback)     \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(formResetCallback)        \
    __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(formStateRestoreCallback)

#define __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME(name) extern FlyString name;
ENUMERATE_CUSTOM_ELEMENT_REACTION_NAMES
#undef __ENUMERATE_CUSTOM_ELEMENT_REACTION_NAME

void initialize_strings();

}
