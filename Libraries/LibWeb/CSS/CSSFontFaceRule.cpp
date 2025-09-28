/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibWeb/Bindings/CSSFontFaceRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFaceRule);

GC::Ref<CSSFontFaceRule> CSSFontFaceRule::create(JS::Realm& realm, GC::Ref<CSSFontFaceDescriptors> style)
{
    return realm.create<CSSFontFaceRule>(realm, style);
}

CSSFontFaceRule::CSSFontFaceRule(JS::Realm& realm, GC::Ref<CSSFontFaceDescriptors> style)
    : CSSRule(realm, Type::FontFace)
    , m_style(style)
{
    m_style->set_parent_rule(*this);
}

void CSSFontFaceRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFaceRule);
    Base::initialize(realm);
}

bool CSSFontFaceRule::is_valid() const
{
    // @font-face rules require a font-family and src descriptor; if either of these are missing, the @font-face rule
    // must not be considered when performing the font matching algorithm.
    // https://drafts.csswg.org/css-fonts-4/#font-face-rule
    return !m_style->descriptor(DescriptorID::FontFamily).is_null()
        && !m_style->descriptor(DescriptorID::Src).is_null();
}

ParsedFontFace CSSFontFaceRule::font_face() const
{
    return ParsedFontFace::from_descriptors(m_style);
}

// https://www.w3.org/TR/cssom/#ref-for-cssfontfacerule
String CSSFontFaceRule::serialized() const
{
    auto& descriptors = *m_style;

    StringBuilder builder;
    // The result of concatenating the following:

    // 1. The string "@font-face {", followed by a single SPACE (U+0020).
    auto is_empty = true;
    builder.append("@font-face { "sv);

    // 2. If the rule’s associated font-family is present, follow these substeps:
    //    The string "font-family:", followed by a single SPACE (U+0020).
    //    followed by the result of performing serialize a string on the rule’s font family name.
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_family = descriptors.descriptor(DescriptorID::FontFamily)) {
        builder.append("font-family: "sv);
        builder.append(descriptors.descriptor(DescriptorID::FontFamily)->to_string(SerializationMode::Normal));
        builder.append(';');
        is_empty = false;
    }

    // 3. If the rule’s associated source list is not empty, a single SPACE (U+0020),
    //    followed by the string "src:", followed by a single SPACE (U+0020).
    //    followed by the result of invoking serialize a comma-separated list on performing serialize a URL or serialize a LOCAL for each source on the source list.
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto sources = descriptors.descriptor(DescriptorID::Src)) {
        if (!is_empty)
            builder.append(' ');
        builder.append("src: "sv);
        builder.append(sources->to_string(SerializationMode::Normal));
        builder.append(';');
        is_empty = false;
    }

    // 4. If rule’s associated unicode-range descriptor is present, a single SPACE (U+0020), followed by the string "unicode-range:", followed by a single SPACE (U+0020), followed by the result of performing serialize a <'unicode-range'>, followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto unicode_range = descriptors.descriptor(DescriptorID::UnicodeRange)) {
        if (!is_empty)
            builder.append(' ');
        builder.append(" unicode-range: "sv);
        builder.append(unicode_range->to_string(SerializationMode::Normal));
        builder.append(';');
        is_empty = false;
    }

    // FIXME: 5. If rule’s associated font-variant descriptor is present, a single SPACE (U+0020),
    // followed by the string "font-variant:", followed by a single SPACE (U+0020),
    // followed by the result of performing serialize a <'font-variant'>,
    // followed by the string ";", i.e., SEMICOLON (U+003B).

    // 6. If rule’s associated font-feature-settings descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-feature-settings:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-feature-settings'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_feature_settings = descriptors.descriptor(DescriptorID::FontFeatureSettings)) {
        if (!is_empty)
            builder.append(' ');
        builder.append(" font-feature-settings: "sv);
        builder.append(font_feature_settings->to_string(SerializationMode::Normal));
        builder.append(";"sv);
        is_empty = false;
    }

    // 7. If rule’s associated font-stretch descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-stretch:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-stretch'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    // NOTE: font-stretch is now an alias for font-width, so we use that instead.
    if (auto font_width = descriptors.descriptor(DescriptorID::FontWidth)) {
        if (!is_empty)
            builder.append(' ');
        builder.append(" font-stretch: "sv);
        builder.append(font_width->to_string(SerializationMode::Normal));
        builder.append(";"sv);
        is_empty = false;
    }

    // 8. If rule’s associated font-weight descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-weight:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-weight'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_weight = descriptors.descriptor(DescriptorID::FontWeight)) {
        if (!is_empty)
            builder.append(' ');
        builder.append(" font-weight: "sv);
        builder.append(font_weight->to_string(SerializationMode::Normal));
        builder.append(";"sv);
        is_empty = false;
    }

    // 9. If rule’s associated font-style descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-style:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-style'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_style = descriptors.descriptor(DescriptorID::FontStyle)) {
        if (!is_empty)
            builder.append(' ');
        builder.append(" font-style: "sv);
        builder.append(font_style->to_string(SerializationMode::Normal));
        builder.append(";"sv);
        is_empty = false;
    }

    // 10. A single SPACE (U+0020), followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D).
    builder.append(" }"sv);

    return MUST(builder.to_string());
}

void CSSFontFaceRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

}
