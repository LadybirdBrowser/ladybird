/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Profile.h>

#import <Cocoa/Cocoa.h>
#import <UI/AppKit/Utilities/ApplicationIcon.h>

namespace Ladybird {

void set_profile_application_icon(WebView::Profile const& profile)
{
    if (profile.identifier() == "default"sv)
        return;

    auto label = profile.is_temporary()
        ? "tmp"sv
        : profile.identifier().substring_view(0, min(profile.identifier().length(), 3uz));
    auto* profile_label = [[[NSString alloc] initWithBytes:label.characters_without_null_termination()
                                                    length:label.length()
                                                  encoding:NSUTF8StringEncoding]
        uppercaseString];
    auto* application_icon = [NSApp applicationIconImage];
    if (!application_icon)
        return;

    NSArray<NSColor*>* badge_colors = @[
        [NSColor colorWithSRGBRed:0.38
                            green:0.52
                             blue:0.96
                            alpha:1],
        [NSColor colorWithSRGBRed:0.49
                            green:0.43
                             blue:0.91
                            alpha:1],
        [NSColor colorWithSRGBRed:0.63
                            green:0.39
                             blue:0.88
                            alpha:1],
        [NSColor colorWithSRGBRed:0.82
                            green:0.36
                             blue:0.65
                            alpha:1],
        [NSColor colorWithSRGBRed:0.16
                            green:0.57
                             blue:0.65
                            alpha:1],
        [NSColor colorWithSRGBRed:0.30
                            green:0.36
                             blue:0.72
                            alpha:1],
    ];
    auto* badge_color = badge_colors[profile.paths().identity.hash() % [badge_colors count]];

    auto icon_size = [application_icon size];
    auto* profile_icon = [NSImage imageWithSize:icon_size
                                        flipped:NO
                                 drawingHandler:^BOOL(NSRect destination) {
                                     [application_icon drawInRect:destination];

                                     auto badge_width = NSWidth(destination) * 0.40;
                                     auto badge_height = NSHeight(destination) * 0.25;
                                     auto badge_margin = NSWidth(destination) * 0.025;
                                     auto badge_rect = NSMakeRect(
                                         NSMaxX(destination) - badge_width - badge_margin,
                                         NSMinY(destination) + badge_margin,
                                         badge_width,
                                         badge_height);
                                     auto* badge = [NSBezierPath bezierPathWithRoundedRect:badge_rect
                                                                                   xRadius:badge_height * 0.28
                                                                                   yRadius:badge_height * 0.28];

                                     [NSGraphicsContext saveGraphicsState];
                                     auto* shadow = [[NSShadow alloc] init];
                                     [shadow setShadowBlurRadius:NSWidth(destination) * 0.025];
                                     [shadow setShadowColor:[NSColor colorWithWhite:0 alpha:0.5]];
                                     [shadow setShadowOffset:NSMakeSize(0, -NSHeight(destination) * 0.01)];
                                     [shadow set];
                                     [badge_color setFill];
                                     [badge fill];
                                     [NSGraphicsContext restoreGraphicsState];

                                     auto* font = [NSFont systemFontOfSize:badge_height * 0.52 weight:NSFontWeightHeavy];
                                     NSDictionary* attributes = @ {
                                         NSFontAttributeName : font,
                                         NSForegroundColorAttributeName : [NSColor whiteColor],
                                     };
                                     auto text_size = [profile_label sizeWithAttributes:attributes];
                                     auto text_origin = NSMakePoint(
                                         NSMidX(badge_rect) - text_size.width / 2,
                                         NSMidY(badge_rect) - text_size.height / 2);
                                     [profile_label drawAtPoint:text_origin withAttributes:attributes];
                                     return YES;
                                 }];
    [NSApp setApplicationIconImage:profile_icon];
}

}
