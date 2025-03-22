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
    "CSS"sv,
    "Clipboard"sv,
    "Compression"sv,
    "ContentSecurityPolicy"sv,
    "Crypto"sv,
    "DOM"sv,
    "DOMURL"sv,
    "Encoding"sv,
    "Fetch"sv,
    "FileAPI"sv,
    "Geometry"sv,
    "HTML"sv,
    "HighResolutionTime"sv,
    "Internals"sv,
    "IntersectionObserver"sv,
    "MathML"sv,
    "MediaSourceExtensions"sv,
    "NavigationTiming"sv,
    "RequestIdleCallback"sv,
    "ResizeObserver"sv,
    "SVG"sv,
    "Selection"sv,
    "ServiceWorker"sv,
    "UIEvents"sv,
    "URLPattern"sv,
    "WebAudio"sv,
    "WebGL"sv,
    "WebIDL"sv,
    "WebSockets"sv,
    "XHR"sv,
};

}
