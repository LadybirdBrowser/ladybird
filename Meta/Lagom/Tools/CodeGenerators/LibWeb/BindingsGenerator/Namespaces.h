/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StringView.h>

namespace IDL {

static constexpr Array libweb_interface_namespaces = {
    "CSS"_sv,
    "Clipboard"_sv,
    "Compression"_sv,
    "ContentSecurityPolicy"_sv,
    "Crypto"_sv,
    "DOM"_sv,
    "DOMURL"_sv,
    "Encoding"_sv,
    "Fetch"_sv,
    "FileAPI"_sv,
    "Geometry"_sv,
    "HTML"_sv,
    "HighResolutionTime"_sv,
    "Internals"_sv,
    "IntersectionObserver"_sv,
    "MathML"_sv,
    "MediaSourceExtensions"_sv,
    "NavigationTiming"_sv,
    "RequestIdleCallback"_sv,
    "ResizeObserver"_sv,
    "SVG"_sv,
    "Selection"_sv,
    "ServiceWorker"_sv,
    "Streams"_sv,
    "UIEvents"_sv,
    "URLPattern"_sv,
    "WebAudio"_sv,
    "WebGL"_sv,
    "WebIDL"_sv,
    "WebSockets"_sv,
    "XHR"_sv,
};

}
