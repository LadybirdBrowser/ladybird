/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSPageDescriptors.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSPageDescriptors.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPageDescriptors);

GC::Ref<CSSPageDescriptors> CSSPageDescriptors::create(JS::Realm& realm, Vector<Descriptor> descriptors)
{
    return realm.create<CSSPageDescriptors>(realm, move(descriptors));
}

CSSPageDescriptors::CSSPageDescriptors(JS::Realm& realm, Vector<Descriptor> descriptors)
    : CSSDescriptors(realm, AtRuleID::Page, move(descriptors))
{
}

CSSPageDescriptors::~CSSPageDescriptors() = default;

void CSSPageDescriptors::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSPageDescriptors);
    Base::initialize(realm);
}

static Utf16String get_property_value_as_utf16(CSSPageDescriptors const& descriptors, Utf16FlyString const& property)
{
    return descriptors.get_property_value(property);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin(Utf16View value)
{
    return set_property("margin"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::margin() const
{
    return get_property_value_as_utf16(*this, "margin"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_top(Utf16View value)
{
    return set_property("margin-top"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::margin_top() const
{
    return get_property_value_as_utf16(*this, "margin-top"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_right(Utf16View value)
{
    return set_property("margin-right"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::margin_right() const
{
    return get_property_value_as_utf16(*this, "margin-right"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_bottom(Utf16View value)
{
    return set_property("margin-bottom"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::margin_bottom() const
{
    return get_property_value_as_utf16(*this, "margin-bottom"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_left(Utf16View value)
{
    return set_property("margin-left"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::margin_left() const
{
    return get_property_value_as_utf16(*this, "margin-left"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_size(Utf16View value)
{
    return set_property("size"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::size() const
{
    return get_property_value_as_utf16(*this, "size"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_page_orientation(Utf16View value)
{
    return set_property("page-orientation"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::page_orientation() const
{
    return get_property_value_as_utf16(*this, "page-orientation"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_marks(Utf16View value)
{
    return set_property("marks"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::marks() const
{
    return get_property_value_as_utf16(*this, "marks"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_bleed(Utf16View value)
{
    return set_property("bleed"_utf16_fly_string, value, u""sv);
}

Utf16String CSSPageDescriptors::bleed() const
{
    return get_property_value_as_utf16(*this, "bleed"_utf16_fly_string);
}

}
