/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::MediaSourceExtensions::EventNames {

#define ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES                  \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(abort)              \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(addsourcebuffer)    \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(bufferedchange)     \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(endstreaming)       \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(error)              \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(removesourcebuffer) \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(sourceclose)        \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(sourceended)        \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(sourceopen)         \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(startstreaming)     \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(update)             \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(updateend)          \
    __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(updatestart)

#define __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(name) extern FlyString name;
ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES
#undef __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE

}
