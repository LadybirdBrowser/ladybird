/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::Platform {

// An agent talks to the decoder over a connection of its own, which only its thread can use.
static thread_local ImageCodecPlugin* s_the;

ImageCodecPlugin::~ImageCodecPlugin() = default;

ImageCodecPlugin& ImageCodecPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

void ImageCodecPlugin::install(ImageCodecPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

void ImageCodecPlugin::shut_down()
{
    delete s_the;
    s_the = nullptr;
}

}
