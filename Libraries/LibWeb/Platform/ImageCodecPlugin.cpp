/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/ImageCodecPlugin.h>

#ifndef LIBWEB_UNITY_ID
#    define LIBWEB_UNITY_ID LIBWEB_UNITY_ID_FALLBACK
#endif

namespace Web::Platform {

namespace {
namespace LIBWEB_UNITY_ID {

static ImageCodecPlugin* s_the;

}
}

ImageCodecPlugin::~ImageCodecPlugin() = default;

ImageCodecPlugin& ImageCodecPlugin::the()
{
    VERIFY(LIBWEB_UNITY_ID::s_the);
    return *LIBWEB_UNITY_ID::s_the;
}

void ImageCodecPlugin::install(ImageCodecPlugin& plugin)
{
    VERIFY(!LIBWEB_UNITY_ID::s_the);
    LIBWEB_UNITY_ID::s_the = &plugin;
}

}
