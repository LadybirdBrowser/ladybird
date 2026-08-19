/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>

namespace Ladybird {

// https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
Optional<String> mime_type_for_pasteboard_type(NSPasteboardType);
NSPasteboardType pasteboard_type_for_mime_type(StringView mime_type);

}
