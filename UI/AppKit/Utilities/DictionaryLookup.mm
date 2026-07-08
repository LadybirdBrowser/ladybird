/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibWebView/DictionaryLookup.h>

#import <UI/AppKit/Utilities/DictionaryLookup.h>

namespace Ladybird {

static NSString* ns_string_from_ak_string(String const& string)
{
    auto view = string.bytes_as_string_view();
    return [[NSString alloc] initWithBytes:view.characters_without_null_termination()
                                    length:view.length()
                                  encoding:NSUTF8StringEncoding];
}

static NSFontWeight ns_font_weight_from_css_weight(u16 weight)
{
    if (weight >= 900)
        return NSFontWeightBlack;
    if (weight >= 800)
        return NSFontWeightHeavy;
    if (weight >= 700)
        return NSFontWeightBold;
    if (weight >= 600)
        return NSFontWeightSemibold;
    if (weight >= 500)
        return NSFontWeightMedium;
    return NSFontWeightRegular;
}

static NSFont* ns_font_from_dictionary_lookup_text_style(WebView::DictionaryLookupTextStyle const& style)
{
    if (style.ui_point_size <= 0)
        return nil;

    auto traits = style.slope == 0 ? 0 : NSFontItalicTrait;
    auto* family = ns_string_from_ak_string(style.font_family);
    auto* font = [[NSFontManager sharedFontManager] fontWithFamily:family
                                                            traits:traits
                                                            weight:style.weight / 100
                                                              size:style.ui_point_size];
    if (font)
        return font;

    font = [NSFont systemFontOfSize:style.ui_point_size weight:ns_font_weight_from_css_weight(style.weight)];
    if (style.slope != 0)
        font = [[NSFontManager sharedFontManager] convertFont:font toHaveTrait:NSItalicFontMask];
    return font;
}

static NSAttributedString* attributed_string_for_dictionary_lookup(WebView::DictionaryLookup const& lookup)
{
    auto* ns_text = ns_string_from_ak_string(lookup.text);
    if (!lookup.style.has_value())
        return [[NSAttributedString alloc] initWithString:ns_text];

    auto* font = ns_font_from_dictionary_lookup_text_style(*lookup.style);
    if (!font)
        return [[NSAttributedString alloc] initWithString:ns_text];

    return [[NSAttributedString alloc] initWithString:ns_text attributes:@ { NSFontAttributeName : font }];
}

void show_dictionary_lookup(NSView* view, WebView::DictionaryLookup const& lookup, NSPoint position)
{
    if (!view || lookup.text.is_empty())
        return;

    [view showDefinitionForAttributedString:attributed_string_for_dictionary_lookup(lookup) atPoint:position];
}

}
