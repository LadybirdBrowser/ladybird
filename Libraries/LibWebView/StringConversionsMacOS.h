/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWebView/Forward.h>

#import <Foundation/Foundation.h>

namespace WebView {

WEBVIEW_API String ns_string_to_string(NSString*);
WEBVIEW_API NSString* string_to_ns_string(StringView);

// NSString and our UTF-16 strings hold the same code units, lone surrogates included, so offsets carry over unchanged.
WEBVIEW_API Utf16String ns_string_to_utf16_string(NSString*);
WEBVIEW_API NSString* utf16_string_to_ns_string(Utf16View const&);

WEBVIEW_API NSData* string_to_ns_data(StringView);

}
