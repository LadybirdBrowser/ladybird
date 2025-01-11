/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::XHR::EventNames {

#define ENUMERATE_XHR_EVENTS                \
    __ENUMERATE_XHR_EVENT(abort)            \
    __ENUMERATE_XHR_EVENT(error)            \
    __ENUMERATE_XHR_EVENT(load)             \
    __ENUMERATE_XHR_EVENT(loadend)          \
    __ENUMERATE_XHR_EVENT(loadstart)        \
    __ENUMERATE_XHR_EVENT(progress)         \
    __ENUMERATE_XHR_EVENT(readystatechange) \
    __ENUMERATE_XHR_EVENT(timeout)

#define __ENUMERATE_XHR_EVENT(name) extern FlyString name;
ENUMERATE_XHR_EVENTS
#undef __ENUMERATE_XHR_EVENT

}
