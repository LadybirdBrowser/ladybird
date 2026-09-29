/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

void show_dictionary_lookup(NSView*, WebView::DictionaryLookup const&, NSPoint);

}
