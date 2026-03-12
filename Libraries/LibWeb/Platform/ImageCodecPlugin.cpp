/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::Platform {

static ImageCodecPlugin* s_the;

ImageCodecPlugin::~ImageCodecPlugin() = default;

ImageCodecPlugin& ImageCodecPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

bool ImageCodecPlugin::is_initialized()
{
    return s_the != nullptr;
}

void ImageCodecPlugin::install(ImageCodecPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

}
