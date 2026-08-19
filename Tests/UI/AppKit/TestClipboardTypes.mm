/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Application/ClipboardTypes.h>

#include <LibTest/TestCase.h>

TEST_CASE(maps_the_appkit_type_constants)
{
    EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(NSPasteboardTypeString), "text/plain"_string);
    EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(NSPasteboardTypeHTML), "text/html"_string);
    EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(NSPasteboardTypePNG), "image/png"_string);
}

// The general pasteboard reports types as NSString instances distinct from the AppKit constants — so matching them
// with pointer equality drops every representation and a paste from another app silently does nothing. Build equally
// distinct instances here, rather than reading the user's clipboard. Note: -initWithString on an immutable string
// returns the receiver — so it can't be used for this.
TEST_CASE(maps_types_that_are_not_the_constant_instances)
{
    auto* built = [NSString stringWithUTF8String:"public.utf8-plain-text"];
    EXPECT_NE(built, NSPasteboardTypeString);
    EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(built), "text/plain"_string);

    auto* formatted = [NSString stringWithFormat:@"%@", NSPasteboardTypeHTML];
    EXPECT_NE(formatted, NSPasteboardTypeHTML);
    EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(formatted), "text/html"_string);
}

// A smoke test over the shape the read path actually walks. Note: this doesn't stand in for the case above. A
// pasteboard read back in the process that wrote it can hand back the interned constant — in which case pointer
// equality would happen to work here.
TEST_CASE(maps_the_types_a_real_pasteboard_reports)
{
    auto* pasteboard = [NSPasteboard pasteboardWithUniqueName];
    [pasteboard clearContents];
    [pasteboard setData:[@"hello" dataUsingEncoding:NSUTF8StringEncoding] forType:NSPasteboardTypeString];

    bool found_text_plain = false;
    for (NSPasteboardType type : [pasteboard types]) {
        if (auto mime_type = Ladybird::mime_type_for_pasteboard_type(type); mime_type == "text/plain"_string)
            found_text_plain = true;
    }
    [pasteboard releaseGlobally];

    EXPECT(found_text_plain);
}

TEST_CASE(ignores_unknown_types)
{
    EXPECT(!Ladybird::mime_type_for_pasteboard_type(NSPasteboardTypeColor).has_value());
    EXPECT(!Ladybird::mime_type_for_pasteboard_type(@"com.example.private-format").has_value());
}

TEST_CASE(maps_mime_types_back_to_pasteboard_types)
{
    EXPECT([Ladybird::pasteboard_type_for_mime_type("text/plain"sv) isEqualToString:NSPasteboardTypeString]);
    EXPECT([Ladybird::pasteboard_type_for_mime_type("text/html"sv) isEqualToString:NSPasteboardTypeHTML]);
    EXPECT([Ladybird::pasteboard_type_for_mime_type("image/png"sv) isEqualToString:NSPasteboardTypePNG]);
    EXPECT_EQ(Ladybird::pasteboard_type_for_mime_type("application/pdf"sv), nil);
}

TEST_CASE(round_trips_every_supported_mime_type)
{
    for (auto mime_type : { "text/plain"sv, "text/html"sv, "image/png"sv }) {
        auto* pasteboard_type = Ladybird::pasteboard_type_for_mime_type(mime_type);
        EXPECT_NE(pasteboard_type, nil);
        EXPECT_EQ(Ladybird::mime_type_for_pasteboard_type(pasteboard_type), MUST(String::from_utf8(mime_type)));
    }
}
