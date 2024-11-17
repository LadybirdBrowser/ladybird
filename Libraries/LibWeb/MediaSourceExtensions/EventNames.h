/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::MediaSourceExtensions::EventNames {

#define ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES(E) \
    E(abort)                                            \
    E(addsourcebuffer)                                  \
    E(bufferedchange)                                   \
    E(endstreaming)                                     \
    E(error)                                            \
    E(removesourcebuffer)                               \
    E(sourceclose)                                      \
    E(sourceended)                                      \
    E(sourceopen)                                       \
    E(startstreaming)                                   \
    E(update)                                           \
    E(updateend)                                        \
    E(updatestart)

#define __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(name) extern FlyString name;
ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES(__ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE)
#undef __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE

void initialize_strings();

}
