/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/FontFaceState.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC::Ref<CSSDescriptors> CSSFontFaceRule::descriptors() const
{
    return static_cast<CSSDescriptors&>(*declarations());
}

bool CSSFontFaceRule::is_valid() const
{
    // @font-face rules require a font-family and src descriptor; if either of these are missing, the @font-face rule
    // must not be considered when performing the font matching algorithm.
    // https://drafts.csswg.org/css-fonts-4/#font-face-rule
    RustDescriptorBlock descriptors { Parser::ValueParserFFI::rust_descriptor_block_retain(native_rule().payload().descriptors) };
    return !descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::FontFamily)).is_null()
        && !descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Src)).is_null();
}

RefPtr<FontFaceState> CSSFontFaceRule::css_connected_font_face() const
{
    auto* sheet = parent_style_sheet();
    return sheet ? sheet->css_connected_font_face(native_rule().identity()) : nullptr;
}

void CSSFontFaceRule::handle_descriptor_change(Utf16FlyString const& property)
{
    auto font_face = css_connected_font_face();
    if (!font_face)
        return;

    if (!is_valid()) {
        disconnect_font_face();
        return;
    }

    if (property.equals_ignoring_ascii_case("src"_utf16_fly_string)) {
        handle_src_descriptor_change();
        return;
    }

    auto descriptor_affects_font_matching = property.equals_ignoring_ascii_case("font-family"_utf16_fly_string)
        || property.equals_ignoring_ascii_case("font-weight"_utf16_fly_string)
        || property.equals_ignoring_ascii_case("font-style"_utf16_fly_string)
        || property.equals_ignoring_ascii_case("font-width"_utf16_fly_string)
        || property.equals_ignoring_ascii_case("font-stretch"_utf16_fly_string);

    FontComputer* font_computer = nullptr;
    if (descriptor_affects_font_matching) {
        if (auto document = parent_style_sheet() ? parent_style_sheet()->owning_document() : nullptr) {
            font_computer = &document->font_computer();
            font_computer->unregister_font_face(*font_face);
        }
    }

    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // any change made to a @font-face descriptor is immediately reflected in the corresponding FontFace attribute
    font_face->reparse_connected_css_font_face_rule_descriptors();

    if (font_computer) {
        font_computer->register_font_face(*font_face);
        return;
    }

    // A descriptor that leaves the face's matching key alone, such as unicode-range or font-display, still changes
    // what selecting the face answers with, in ways a cascade's fonts alone do not show. Such an edit is rare, so every
    // cascade of the family goes.
    if (!font_face->should_be_registered_with_font_computer())
        return;
    if (auto document = parent_style_sheet() ? parent_style_sheet()->owning_document() : nullptr)
        document->font_computer().did_load_font(font_face->family_name());
}

// https://drafts.csswg.org/css-font-loading/#font-face-css-connection
void CSSFontFaceRule::handle_src_descriptor_change()
{
    // If a @font-face rule has its src descriptor changed to a new value, the original connected FontFace object must
    // stop being CSS-connected. A new FontFace reflecting its new src must be created and CSS-connected to the
    // @font-face.

    if (!css_connected_font_face())
        return;

    disconnect_font_face();

    auto* style_sheet = parent_style_sheet();
    if (!style_sheet)
        return;

    auto document = style_sheet->owning_document();
    if (!document)
        return;

    auto new_font_face = FontFaceState::create_css_connected(HTML::relevant_realm(*document), native_rule().identity(), *style_sheet);
    document->fonts()->add_css_connected_font(new_font_face);
}

// https://drafts.csswg.org/css-font-loading/#font-face-css-connection
// If a @font-face rule is removed from the document, its corresponding FontFace object is no longer CSS-connected.
// The connection is not restorable by any means (but adding the @font-face back to the stylesheet will create a
// brand new FontFace object which is CSS-connected).
void CSSFontFaceRule::disconnect_font_face()
{
    auto font_face = css_connected_font_face();
    if (!font_face)
        return;

    font_face->disconnect_from_css_rule();
}

}
