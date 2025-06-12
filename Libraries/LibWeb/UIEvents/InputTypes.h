/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::UIEvents::InputTypes {

// https://w3c.github.io/input-events/#interface-InputEvent-Attributes
#define ENUMERATE_INPUT_TYPES                     \
    __ENUMERATE_INPUT_TYPE(deleteContentBackward) \
    __ENUMERATE_INPUT_TYPE(deleteContentForward)  \
    __ENUMERATE_INPUT_TYPE(insertLineBreak)       \
    __ENUMERATE_INPUT_TYPE(insertParagraph)       \
    __ENUMERATE_INPUT_TYPE(insertText)

#define __ENUMERATE_INPUT_TYPE(name) extern FlyString name;
ENUMERATE_INPUT_TYPES
#undef __ENUMERATE_INPUT_TYPE

}
