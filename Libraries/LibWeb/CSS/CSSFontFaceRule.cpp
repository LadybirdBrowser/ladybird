/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
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

GC::Ref<CSSFontFaceRule> CSSFontFaceRule::create(JS::Realm& realm, ParsedFontFace&& font_face)
{
    return realm.create<CSSFontFaceRule>(realm, move(font_face));
}

CSSFontFaceRule::CSSFontFaceRule(JS::Realm& realm, ParsedFontFace&& font_face)
    : CSSRule(realm, Type::FontFace)
    , m_font_face(move(font_face))
{
}

void CSSFontFaceRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFaceRule);
}

CSSStyleDeclaration* CSSFontFaceRule::style()
{
    // FIXME: Return a CSSStyleDeclaration subclass that directs changes to the ParsedFontFace.
    return nullptr;
}

// https://www.w3.org/TR/cssom/#ref-for-cssfontfacerule
String CSSFontFaceRule::serialized() const
{
    StringBuilder builder;
    // The result of concatenating the following:

    // 1. The string "@font-face {", followed by a single SPACE (U+0020).
    builder.append("@font-face { "sv);

    // 2. The string "font-family:", followed by a single SPACE (U+0020).
    builder.append("font-family: "sv);

    // 3. The result of performing serialize a string on the rule’s font family name.
    serialize_a_string(builder, m_font_face.font_family());

    // 4. The string ";", i.e., SEMICOLON (U+003B).
    builder.append(';');

    // 5. If the rule’s associated source list is not empty, follow these substeps:
    if (!m_font_face.sources().is_empty()) {
        // 1. A single SPACE (U+0020), followed by the string "src:", followed by a single SPACE (U+0020).
        builder.append(" src: "sv);

        // 2. The result of invoking serialize a comma-separated list on performing serialize a URL or serialize a LOCAL for each source on the source list.
        serialize_a_comma_separated_list(builder, m_font_face.sources(), [&](StringBuilder& builder, ParsedFontFace::Source source) -> void {
            source.local_or_url.visit(
                [&builder](URL::URL const& url) {
                    serialize_a_url(builder, url.to_string());
                },
                [&builder](FlyString const& local) {
                    // https://drafts.csswg.org/cssom-1/#serialize-a-local
                    // To serialize a LOCAL means to create a string represented by "local(", followed by the serialization of the LOCAL as a string, followed by ")".
                    builder.appendff("local({})", serialize_a_string(local));
                });

            // NOTE: No spec currently exists for format()
            if (source.format.has_value()) {
                builder.append(" format("sv);
                serialize_a_string(builder, source.format.value());
                builder.append(")"sv);
            }
        });

        // 3. The string ";", i.e., SEMICOLON (U+003B).
        builder.append(';');
    }

    // 6. If rule’s associated unicode-range descriptor is present, a single SPACE (U+0020), followed by the string "unicode-range:", followed by a single SPACE (U+0020), followed by the result of performing serialize a <'unicode-range'>, followed by the string ";", i.e., SEMICOLON (U+003B).
    builder.append(" unicode-range: "sv);
    serialize_unicode_ranges(builder, m_font_face.unicode_ranges());
    builder.append(';');

    // FIXME: 7. If rule’s associated font-variant descriptor is present, a single SPACE (U+0020),
    // followed by the string "font-variant:", followed by a single SPACE (U+0020),
    // followed by the result of performing serialize a <'font-variant'>,
    // followed by the string ";", i.e., SEMICOLON (U+003B).

    // 8. If rule’s associated font-feature-settings descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-feature-settings:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-feature-settings'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    if (m_font_face.font_feature_settings().has_value()) {
        auto const& feature_settings = m_font_face.font_feature_settings().value();
        builder.append(" font-feature-settings: "sv);
        // NOTE: We sort the tags during parsing, so they're already in the correct order.
        bool first = true;
        for (auto const& [key, value] : feature_settings) {
            if (first) {
                first = false;
            } else {
                builder.append(", "sv);
            }

            serialize_a_string(builder, key);
            // NOTE: 1 is the default value, so don't serialize it.
            if (value != 1)
                builder.appendff(" {}", value);
        }
        builder.append(";"sv);
    }

    // 9. If rule’s associated font-stretch descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-stretch:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-stretch'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    // NOTE: font-stretch is now an alias for font-width, so we use that instead.
    if (m_font_face.width().has_value()) {
        builder.append(" font-width: "sv);
        // NOTE: font-width is supposed to always be serialized as a percentage.
        //       Right now, it's stored as a Gfx::FontWidth value, so we have to lossily convert it back.
        float percentage = 100.0f;
        switch (m_font_face.width().value()) {
        case Gfx::FontWidth::UltraCondensed:
            percentage = 50.0f;
            break;
        case Gfx::FontWidth::ExtraCondensed:
            percentage = 62.5f;
            break;
        case Gfx::FontWidth::Condensed:
            percentage = 75.0f;
            break;
        case Gfx::FontWidth::SemiCondensed:
            percentage = 87.5f;
            break;
        case Gfx::FontWidth::Normal:
            percentage = 100.0f;
            break;
        case Gfx::FontWidth::SemiExpanded:
            percentage = 112.5f;
            break;
        case Gfx::FontWidth::Expanded:
            percentage = 125.0f;
            break;
        case Gfx::FontWidth::ExtraExpanded:
            percentage = 150.0f;
            break;
        case Gfx::FontWidth::UltraExpanded:
            percentage = 200.0f;
            break;
        default:
            break;
        }
        builder.appendff("{}%", percentage);
        builder.append(";"sv);
    }

    // 10. If rule’s associated font-weight descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-weight:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-weight'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (m_font_face.weight().has_value()) {
        auto weight = m_font_face.weight().value();
        builder.append(" font-weight: "sv);
        if (weight == 400)
            builder.append("normal"sv);
        else if (weight == 700)
            builder.append("bold"sv);
        else
            builder.appendff("{}", weight);
        builder.append(";"sv);
    }

    // 11. If rule’s associated font-style descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-style:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-style'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (m_font_face.slope().has_value()) {
        auto slope = m_font_face.slope().value();
        builder.append(" font-style: "sv);
        if (slope == Gfx::name_to_slope("Normal"sv))
            builder.append("normal"sv);
        else if (slope == Gfx::name_to_slope("Italic"sv))
            builder.append("italic"sv);
        else {
            dbgln("FIXME: CSSFontFaceRule::serialized() does not support slope {}", slope);
            builder.append("italic"sv);
        }
        builder.append(";"sv);
    }

    // 12. A single SPACE (U+0020), followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D).
    builder.append(" }"sv);

    return MUST(builder.to_string());
}

}
