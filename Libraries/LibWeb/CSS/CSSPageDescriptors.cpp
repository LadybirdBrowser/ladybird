/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSPageDescriptorsPrototype.h>
#include <LibWeb/CSS/CSSPageDescriptors.h>

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

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin(StringView value)
{
    return set_property("margin"_sv, value, ""_sv);
}

String CSSPageDescriptors::margin() const
{
    return get_property_value("margin"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_top(StringView value)
{
    return set_property("margin-top"_sv, value, ""_sv);
}

String CSSPageDescriptors::margin_top() const
{
    return get_property_value("margin-top"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_right(StringView value)
{
    return set_property("margin-right"_sv, value, ""_sv);
}

String CSSPageDescriptors::margin_right() const
{
    return get_property_value("margin-right"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_bottom(StringView value)
{
    return set_property("margin-bottom"_sv, value, ""_sv);
}

String CSSPageDescriptors::margin_bottom() const
{
    return get_property_value("margin-bottom"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_left(StringView value)
{
    return set_property("margin-left"_sv, value, ""_sv);
}

String CSSPageDescriptors::margin_left() const
{
    return get_property_value("margin-left"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_size(StringView value)
{
    return set_property("size"_sv, value, ""_sv);
}

String CSSPageDescriptors::size() const
{
    return get_property_value("size"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_page_orientation(StringView value)
{
    return set_property("page-orientation"_sv, value, ""_sv);
}

String CSSPageDescriptors::page_orientation() const
{
    return get_property_value("page-orientation"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_marks(StringView value)
{
    return set_property("marks"_sv, value, ""_sv);
}

String CSSPageDescriptors::marks() const
{
    return get_property_value("marks"_sv);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_bleed(StringView value)
{
    return set_property("bleed"_sv, value, ""_sv);
}

String CSSPageDescriptors::bleed() const
{
    return get_property_value("bleed"_sv);
}

}
