/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::UIEvents::EventNames {

#define ENUMERATE_UI_EVENTS                  \
    __ENUMERATE_UI_EVENT(auxclick)           \
    __ENUMERATE_UI_EVENT(beforeinput)        \
    __ENUMERATE_UI_EVENT(click)              \
    __ENUMERATE_UI_EVENT(contextmenu)        \
    __ENUMERATE_UI_EVENT(dblclick)           \
    __ENUMERATE_UI_EVENT(gotpointercapture)  \
    __ENUMERATE_UI_EVENT(input)              \
    __ENUMERATE_UI_EVENT(keydown)            \
    __ENUMERATE_UI_EVENT(keypress)           \
    __ENUMERATE_UI_EVENT(keyup)              \
    __ENUMERATE_UI_EVENT(lostpointercapture) \
    __ENUMERATE_UI_EVENT(mousedown)          \
    __ENUMERATE_UI_EVENT(mouseenter)         \
    __ENUMERATE_UI_EVENT(mouseleave)         \
    __ENUMERATE_UI_EVENT(mousemove)          \
    __ENUMERATE_UI_EVENT(mouseout)           \
    __ENUMERATE_UI_EVENT(mouseover)          \
    __ENUMERATE_UI_EVENT(mouseup)            \
    __ENUMERATE_UI_EVENT(pointercancel)      \
    __ENUMERATE_UI_EVENT(pointerdown)        \
    __ENUMERATE_UI_EVENT(pointerenter)       \
    __ENUMERATE_UI_EVENT(pointerleave)       \
    __ENUMERATE_UI_EVENT(pointermove)        \
    __ENUMERATE_UI_EVENT(pointerout)         \
    __ENUMERATE_UI_EVENT(pointerover)        \
    __ENUMERATE_UI_EVENT(pointerrawupdate)   \
    __ENUMERATE_UI_EVENT(pointerup)          \
    __ENUMERATE_UI_EVENT(resize)             \
    __ENUMERATE_UI_EVENT(wheel)

#define __ENUMERATE_UI_EVENT(name) extern FlyString name;
ENUMERATE_UI_EVENTS
#undef __ENUMERATE_UI_EVENT

void initialize_strings();

}
