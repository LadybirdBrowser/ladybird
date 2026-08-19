/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Application/ClipboardTypes.h>

namespace Ladybird {

Optional<String> mime_type_for_pasteboard_type(NSPasteboardType type)
{
    if ([type isEqualToString:NSPasteboardTypeString])
        return "text/plain"_string;
    if ([type isEqualToString:NSPasteboardTypeHTML])
        return "text/html"_string;
    if ([type isEqualToString:NSPasteboardTypePNG])
        return "image/png"_string;
    return {};
}

NSPasteboardType pasteboard_type_for_mime_type(StringView mime_type)
{
    if (mime_type == "text/plain"sv)
        return NSPasteboardTypeString;
    if (mime_type == "text/html"sv)
        return NSPasteboardTypeHTML;
    if (mime_type == "image/png"sv)
        return NSPasteboardTypePNG;
    return nil;
}

}
