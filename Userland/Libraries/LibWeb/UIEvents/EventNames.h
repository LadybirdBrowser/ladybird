/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FlyString.h>

namespace Web::UIEvents::EventNames {

// FIXME: This is not all of the events

#define ENUMERATE_UI_EVENTS           \
    __ENUMERATE_UI_EVENT(auxclick)    \
    __ENUMERATE_UI_EVENT(beforeinput) \
    __ENUMERATE_UI_EVENT(click)       \
    __ENUMERATE_UI_EVENT(contextmenu) \
    __ENUMERATE_UI_EVENT(dblclick)    \
    __ENUMERATE_UI_EVENT(input)       \
    __ENUMERATE_UI_EVENT(keydown)     \
    __ENUMERATE_UI_EVENT(keypress)    \
    __ENUMERATE_UI_EVENT(keyup)       \
    __ENUMERATE_UI_EVENT(mousedown)   \
    __ENUMERATE_UI_EVENT(mouseenter)  \
    __ENUMERATE_UI_EVENT(mouseleave)  \
    __ENUMERATE_UI_EVENT(mousemove)   \
    __ENUMERATE_UI_EVENT(mouseout)    \
    __ENUMERATE_UI_EVENT(mouseover)   \
    __ENUMERATE_UI_EVENT(mouseup)     \
    __ENUMERATE_UI_EVENT(resize)      \
    __ENUMERATE_UI_EVENT(wheel)

#define __ENUMERATE_UI_EVENT(name) extern FlyString name;
ENUMERATE_UI_EVENTS
#undef __ENUMERATE_UI_EVENT

void initialize_strings();

}
