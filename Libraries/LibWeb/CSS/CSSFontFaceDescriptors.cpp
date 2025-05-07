/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSFontFaceDescriptorsPrototype.h>
#include <LibWeb/CSS/CSSFontFaceDescriptors.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFaceDescriptors);

GC::Ref<CSSFontFaceDescriptors> CSSFontFaceDescriptors::create(JS::Realm& realm, Vector<Descriptor> descriptors)
{
    return realm.create<CSSFontFaceDescriptors>(realm, move(descriptors));
}

CSSFontFaceDescriptors::CSSFontFaceDescriptors(JS::Realm& realm, Vector<Descriptor> descriptors)
    : CSSDescriptors(realm, AtRuleID::FontFace, move(descriptors))
{
}

CSSFontFaceDescriptors::~CSSFontFaceDescriptors() = default;

void CSSFontFaceDescriptors::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFaceDescriptors);
    Base::initialize(realm);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_ascent_override(StringView value)
{
    return set_property("ascent-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::ascent_override() const
{
    return get_property_value("ascent-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_descent_override(StringView value)
{
    return set_property("descent-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::descent_override() const
{
    return get_property_value("descent-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_display(StringView value)
{
    return set_property("font-display"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_display() const
{
    return get_property_value("font-display"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_family(StringView value)
{
    return set_property("font-family"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_family() const
{
    return get_property_value("font-family"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_feature_settings(StringView value)
{
    return set_property("font-feature-settings"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_feature_settings() const
{
    return get_property_value("font-feature-settings"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_language_override(StringView value)
{
    return set_property("font-language-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_language_override() const
{
    return get_property_value("font-language-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_named_instance(StringView value)
{
    return set_property("font-named-instance"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_named_instance() const
{
    return get_property_value("font-named-instance"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_style(StringView value)
{
    return set_property("font-style"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_style() const
{
    return get_property_value("font-style"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_variation_settings(StringView value)
{
    return set_property("font-variation-settings"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_variation_settings() const
{
    return get_property_value("font-variation-settings"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_weight(StringView value)
{
    return set_property("font-weight"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_weight() const
{
    return get_property_value("font-weight"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_width(StringView value)
{
    return set_property("font-width"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_width() const
{
    return get_property_value("font-width"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_line_gap_override(StringView value)
{
    return set_property("line-gap-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::line_gap_override() const
{
    return get_property_value("line-gap-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_src(StringView value)
{
    return set_property("src"sv, value, ""sv);
}

String CSSFontFaceDescriptors::src() const
{
    return get_property_value("src"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_unicode_range(StringView value)
{
    return set_property("unicode-range"sv, value, ""sv);
}

String CSSFontFaceDescriptors::unicode_range() const
{
    return get_property_value("unicode-range"sv);
}

}
