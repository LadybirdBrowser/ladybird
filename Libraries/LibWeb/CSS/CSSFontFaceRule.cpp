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
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
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
    // AD-HOC: We add the in the below AD-HOC block to avoid an extra space if there is no font-family descriptor.
    builder.append("@font-face {"sv);

    // AD-HOC: We don't necessary always have a font-family descriptor as the spec assumes,
    //         see https://github.com/w3c/csswg-drafts/issues/13323

    if (auto font_family = descriptors.descriptor(DescriptorID::FontFamily); !font_family.is_null()) {
        builder.append(' ');

        // 2. The string "font-family:", followed by a single SPACE (U+0020).
        builder.append("font-family: "sv);

        // 3. The result of performing serialize a string on the rule’s font family name.
        descriptors.descriptor(DescriptorID::FontFamily)->serialize(builder, SerializationMode::Normal);

        // 4. The string ";", i.e., SEMICOLON (U+003B).
        builder.append(';');
    }

    // 5. If the rule’s associated source list is not empty, follow these substeps:
    if (auto sources = descriptors.descriptor(DescriptorID::Src)) {
        // 1. A single SPACE (U+0020), followed by the string "src:", followed by a single SPACE (U+0020).
        builder.append(" src: "sv);

        // 2. The result of invoking serialize a comma-separated list on performing serialize a URL or serialize a LOCAL for each source on the source list.
        sources->serialize(builder, SerializationMode::Normal);

        // 3. The string ";", i.e., SEMICOLON (U+003B).
        builder.append(';');
    }

    // 6. If rule’s associated unicode-range descriptor is present, a single SPACE (U+0020), followed by the string "unicode-range:", followed by a single SPACE (U+0020), followed by the result of performing serialize a <'unicode-range'>, followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto unicode_range = descriptors.descriptor(DescriptorID::UnicodeRange)) {
        builder.append(" unicode-range: "sv);
        unicode_range->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    // FIXME: 7. If rule’s associated font-variant descriptor is present, a single SPACE (U+0020),
    // followed by the string "font-variant:", followed by a single SPACE (U+0020),
    // followed by the result of performing serialize a <'font-variant'>,
    // followed by the string ";", i.e., SEMICOLON (U+003B).

    // 8. If rule’s associated font-feature-settings descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-feature-settings:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-feature-settings'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_feature_settings = descriptors.descriptor(DescriptorID::FontFeatureSettings)) {
        builder.append(" font-feature-settings: "sv);
        font_feature_settings->serialize(builder, SerializationMode::Normal);
        builder.append(";"sv);
    }

    // 9. If rule’s associated font-stretch descriptor is present, a single SPACE (U+0020),
    //    followed by the string "font-stretch:", followed by a single SPACE (U+0020),
    //    followed by the result of performing serialize a <'font-stretch'>,
    //    followed by the string ";", i.e., SEMICOLON (U+003B).
    // NOTE: font-stretch is now an alias for font-width, so we use that instead.
    if (auto font_width = descriptors.descriptor(DescriptorID::FontWidth)) {
        builder.append(" font-stretch: "sv);
        font_width->serialize(builder, SerializationMode::Normal);
        builder.append(";"sv);
    }

    // 10. If rule’s associated font-weight descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-weight:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-weight'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_weight = descriptors.descriptor(DescriptorID::FontWeight)) {
        builder.append(" font-weight: "sv);
        font_weight->serialize(builder, SerializationMode::Normal);
        builder.append(";"sv);
    }

    // 11. If rule’s associated font-style descriptor is present, a single SPACE (U+0020),
    //     followed by the string "font-style:", followed by a single SPACE (U+0020),
    //     followed by the result of performing serialize a <'font-style'>,
    //     followed by the string ";", i.e., SEMICOLON (U+003B).
    if (auto font_style = descriptors.descriptor(DescriptorID::FontStyle)) {
        builder.append(" font-style: "sv);
        font_style->serialize(builder, SerializationMode::Normal);
        builder.append(";"sv);
    }

    // 12. A single SPACE (U+0020), followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D).
    builder.append(" }"sv);

    return MUST(builder.to_string());
}

void CSSFontFaceRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
    visitor.visit(m_css_connected_font_face);
}

void CSSFontFaceRule::handle_descriptor_change(FlyString const& property)
{
    if (!m_css_connected_font_face)
        return;

    if (!is_valid()) {
        disconnect_font_face();
        return;
    }

    if (property.equals_ignoring_ascii_case("src"sv))
        handle_src_descriptor_change();

    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // any change made to a @font-face descriptor is immediately reflected in the corresponding FontFace attribute
    m_css_connected_font_face->reparse_connected_css_font_face_rule_descriptors();
}

// https://drafts.csswg.org/css-font-loading/#font-face-css-connection
void CSSFontFaceRule::handle_src_descriptor_change()
{
    // If a @font-face rule has its src descriptor changed to a new value, the original connected FontFace object must
    // stop being CSS-connected. A new FontFace reflecting its new src must be created and CSS-connected to the
    // @font-face.

    if (!m_css_connected_font_face)
        return;

    disconnect_font_face();

    auto* style_sheet = parent_style_sheet();
    if (!style_sheet)
        return;

    auto document = style_sheet->owning_document();
    if (!document)
        return;

    auto new_font_face = FontFace::create_css_connected(realm(), *this);
    document->fonts()->add_css_connected_font(new_font_face);
}

void CSSFontFaceRule::disconnect_font_face()
{
    if (!m_css_connected_font_face)
        return;

    m_css_connected_font_face->disconnect_from_css_rule();

    if (auto* style_sheet = parent_style_sheet()) {
        if (auto document = style_sheet->owning_document())
            document->fonts()->delete_(m_css_connected_font_face);
    }

    m_css_connected_font_face = nullptr;
}

void CSSFontFaceRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Loading state: {}\n", CSSStyleSheet::loading_state_name(loading_state()));

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Valid: {}\n", is_valid());
    dump_descriptors(builder, descriptors(), indent_levels + 1);
}

void CSSFontFaceRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    if (m_parent_style_sheet)
        m_parent_style_sheet->remove_critical_subresource(*this);

    Base::set_parent_style_sheet(parent_style_sheet);

    if (m_parent_style_sheet)
        m_parent_style_sheet->add_critical_subresource(*this);
}

}
