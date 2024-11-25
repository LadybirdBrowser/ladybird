/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::ContentSecurityPolicy::Directives::Names {

#define ENUMERATE_DIRECTIVE_NAMES                                 \
    __ENUMERATE_DIRECTIVE_NAME(BaseUri, "base-uri")               \
    __ENUMERATE_DIRECTIVE_NAME(ChildSrc, "child-src")             \
    __ENUMERATE_DIRECTIVE_NAME(ConnectSrc, "connect-src")         \
    __ENUMERATE_DIRECTIVE_NAME(DefaultSrc, "default-src")         \
    __ENUMERATE_DIRECTIVE_NAME(FontSrc, "font-src")               \
    __ENUMERATE_DIRECTIVE_NAME(FormAction, "form-action")         \
    __ENUMERATE_DIRECTIVE_NAME(FrameAncestors, "frame-ancestors") \
    __ENUMERATE_DIRECTIVE_NAME(FrameSrc, "frame-src")             \
    __ENUMERATE_DIRECTIVE_NAME(ImgSrc, "img-src")                 \
    __ENUMERATE_DIRECTIVE_NAME(ManifestSrc, "manifest-src")       \
    __ENUMERATE_DIRECTIVE_NAME(MediaSrc, "media-src")             \
    __ENUMERATE_DIRECTIVE_NAME(ObjectSrc, "object-src")           \
    __ENUMERATE_DIRECTIVE_NAME(ReportTo, "report-to")             \
    __ENUMERATE_DIRECTIVE_NAME(ReportUri, "report-uri")           \
    __ENUMERATE_DIRECTIVE_NAME(Sandbox, "sandbox")                \
    __ENUMERATE_DIRECTIVE_NAME(ScriptSrc, "script-src")           \
    __ENUMERATE_DIRECTIVE_NAME(ScriptSrcElem, "script-src-elem")  \
    __ENUMERATE_DIRECTIVE_NAME(ScriptSrcAttr, "script-src-attr")  \
    __ENUMERATE_DIRECTIVE_NAME(StyleSrc, "style-src")             \
    __ENUMERATE_DIRECTIVE_NAME(StyleSrcElem, "style-src-elem")    \
    __ENUMERATE_DIRECTIVE_NAME(StyleSrcAttr, "style-src-attr")    \
    __ENUMERATE_DIRECTIVE_NAME(WebRTC, "webrtc")                  \
    __ENUMERATE_DIRECTIVE_NAME(WorkerSrc, "worker-src")

#define __ENUMERATE_DIRECTIVE_NAME(name, value) extern FlyString name;
ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME

void initialize_strings();

}
