/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFaceDescriptors);

GC::Ref<CSSFontFaceDescriptors> CSSFontFaceDescriptors::create(Vector<Descriptor> descriptors)
{
    return GC::Heap::the().allocate<CSSFontFaceDescriptors>(move(descriptors));
}

CSSFontFaceDescriptors::CSSFontFaceDescriptors(Vector<Descriptor> descriptors)
    : CSSDescriptors(AtRuleID::FontFace, move(descriptors))
{
}

CSSFontFaceDescriptors::~CSSFontFaceDescriptors() = default;

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_property(JS::Realm& realm, Utf16FlyString const& property, StringView value, StringView priority)
{
    TRY(Base::set_property(realm, property, value, priority));

    if (auto* font_face_rule = as_if<CSSFontFaceRule>(parent_rule().ptr()))
        font_face_rule->handle_descriptor_change(property);

    return {};
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_ascent_override(JS::Realm& realm, StringView value)
{
    return set_property(realm, "ascent-override"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::ascent_override() const
{
    return get_property_value("ascent-override"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_descent_override(JS::Realm& realm, StringView value)
{
    return set_property(realm, "descent-override"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::descent_override() const
{
    return get_property_value("descent-override"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_display(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-display"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_display() const
{
    return get_property_value("font-display"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_family(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-family"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_family() const
{
    return get_property_value("font-family"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_feature_settings(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-feature-settings"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_feature_settings() const
{
    return get_property_value("font-feature-settings"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_language_override(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-language-override"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_language_override() const
{
    return get_property_value("font-language-override"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_named_instance(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-named-instance"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_named_instance() const
{
    return get_property_value("font-named-instance"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_style(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-style"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_style() const
{
    return get_property_value("font-style"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_variation_settings(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-variation-settings"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_variation_settings() const
{
    return get_property_value("font-variation-settings"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_weight(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-weight"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_weight() const
{
    return get_property_value("font-weight"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_width(JS::Realm& realm, StringView value)
{
    return set_property(realm, "font-width"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::font_width() const
{
    return get_property_value("font-width"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_line_gap_override(JS::Realm& realm, StringView value)
{
    return set_property(realm, "line-gap-override"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::line_gap_override() const
{
    return get_property_value("line-gap-override"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_src(JS::Realm& realm, StringView value)
{
    return set_property(realm, "src"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::src() const
{
    return get_property_value("src"_fly_string);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_unicode_range(JS::Realm& realm, StringView value)
{
    return set_property(realm, "unicode-range"_fly_string, value, ""sv);
}

String CSSFontFaceDescriptors::unicode_range() const
{
    return get_property_value("unicode-range"_fly_string);
}

}
