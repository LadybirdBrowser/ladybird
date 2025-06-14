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
    return set_property("ascent-override"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::ascent_override() const
{
    return get_property_value("ascent-override"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_descent_override(StringView value)
{
    return set_property("descent-override"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::descent_override() const
{
    return get_property_value("descent-override"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_display(StringView value)
{
    return set_property("font-display"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_display() const
{
    return get_property_value("font-display"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_family(StringView value)
{
    return set_property("font-family"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_family() const
{
    return get_property_value("font-family"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_feature_settings(StringView value)
{
    return set_property("font-feature-settings"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_feature_settings() const
{
    return get_property_value("font-feature-settings"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_language_override(StringView value)
{
    return set_property("font-language-override"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_language_override() const
{
    return get_property_value("font-language-override"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_named_instance(StringView value)
{
    return set_property("font-named-instance"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_named_instance() const
{
    return get_property_value("font-named-instance"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_style(StringView value)
{
    return set_property("font-style"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_style() const
{
    return get_property_value("font-style"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_variation_settings(StringView value)
{
    return set_property("font-variation-settings"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_variation_settings() const
{
    return get_property_value("font-variation-settings"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_weight(StringView value)
{
    return set_property("font-weight"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_weight() const
{
    return get_property_value("font-weight"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_width(StringView value)
{
    return set_property("font-width"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::font_width() const
{
    return get_property_value("font-width"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_line_gap_override(StringView value)
{
    return set_property("line-gap-override"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::line_gap_override() const
{
    return get_property_value("line-gap-override"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_src(StringView value)
{
    return set_property("src"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::src() const
{
    return get_property_value("src"_sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_unicode_range(StringView value)
{
    return set_property("unicode-range"_sv, value, ""_sv);
}

String CSSFontFaceDescriptors::unicode_range() const
{
    return get_property_value("unicode-range"_sv);
}

}
