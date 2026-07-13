/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSFontFaceDescriptors.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

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

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority)
{
    TRY(Base::set_property(property, value, priority));

    if (!property.is_ascii())
        return {};

    if (auto* font_face_rule = as_if<CSSFontFaceRule>(parent_rule().ptr()))
        font_face_rule->handle_descriptor_change(property);

    return {};
}

static Utf16String get_property_value_as_utf16(CSSFontFaceDescriptors const& descriptors, Utf16FlyString const& property)
{
    return descriptors.get_property_value(property);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_ascent_override(Utf16View value)
{
    return set_property("ascent-override"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::ascent_override() const
{
    return get_property_value_as_utf16(*this, "ascent-override"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_descent_override(Utf16View value)
{
    return set_property("descent-override"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::descent_override() const
{
    return get_property_value_as_utf16(*this, "descent-override"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_display(Utf16View value)
{
    return set_property("font-display"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_display() const
{
    return get_property_value_as_utf16(*this, "font-display"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_family(Utf16View value)
{
    return set_property("font-family"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_family() const
{
    return get_property_value_as_utf16(*this, "font-family"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_feature_settings(Utf16View value)
{
    return set_property("font-feature-settings"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_feature_settings() const
{
    return get_property_value_as_utf16(*this, "font-feature-settings"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_language_override(Utf16View value)
{
    return set_property("font-language-override"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_language_override() const
{
    return get_property_value_as_utf16(*this, "font-language-override"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_named_instance(Utf16View value)
{
    return set_property("font-named-instance"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_named_instance() const
{
    return get_property_value_as_utf16(*this, "font-named-instance"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_style(Utf16View value)
{
    return set_property("font-style"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_style() const
{
    return get_property_value_as_utf16(*this, "font-style"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_variation_settings(Utf16View value)
{
    return set_property("font-variation-settings"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_variation_settings() const
{
    return get_property_value_as_utf16(*this, "font-variation-settings"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_weight(Utf16View value)
{
    return set_property("font-weight"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_weight() const
{
    return get_property_value_as_utf16(*this, "font-weight"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_width(Utf16View value)
{
    return set_property("font-width"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::font_width() const
{
    return get_property_value_as_utf16(*this, "font-width"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_line_gap_override(Utf16View value)
{
    return set_property("line-gap-override"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::line_gap_override() const
{
    return get_property_value_as_utf16(*this, "line-gap-override"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_src(Utf16View value)
{
    return set_property("src"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::src() const
{
    return get_property_value_as_utf16(*this, "src"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_unicode_range(Utf16View value)
{
    return set_property("unicode-range"_utf16_fly_string, value, u""sv);
}

Utf16String CSSFontFaceDescriptors::unicode_range() const
{
    return get_property_value_as_utf16(*this, "unicode-range"_utf16_fly_string);
}

}
