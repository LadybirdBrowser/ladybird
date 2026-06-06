/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSPageDescriptors.h>
#include <LibWeb/CSS/CSSPageDescriptors.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPageDescriptors);

GC::Ref<CSSPageDescriptors> CSSPageDescriptors::create(Vector<Descriptor> descriptors)
{
    return GC::Heap::the().allocate<CSSPageDescriptors>(move(descriptors));
}

CSSPageDescriptors::CSSPageDescriptors(Vector<Descriptor> descriptors)
    : CSSDescriptors(AtRuleID::Page, move(descriptors))
{
}

CSSPageDescriptors::~CSSPageDescriptors() = default;

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin(JS::Realm& realm, StringView value)
{
    return set_property(realm, "margin"_fly_string, value, ""sv);
}

String CSSPageDescriptors::margin() const
{
    return get_property_value("margin"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_top(JS::Realm& realm, StringView value)
{
    return set_property(realm, "margin-top"_fly_string, value, ""sv);
}

String CSSPageDescriptors::margin_top() const
{
    return get_property_value("margin-top"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_right(JS::Realm& realm, StringView value)
{
    return set_property(realm, "margin-right"_fly_string, value, ""sv);
}

String CSSPageDescriptors::margin_right() const
{
    return get_property_value("margin-right"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_bottom(JS::Realm& realm, StringView value)
{
    return set_property(realm, "margin-bottom"_fly_string, value, ""sv);
}

String CSSPageDescriptors::margin_bottom() const
{
    return get_property_value("margin-bottom"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_margin_left(JS::Realm& realm, StringView value)
{
    return set_property(realm, "margin-left"_fly_string, value, ""sv);
}

String CSSPageDescriptors::margin_left() const
{
    return get_property_value("margin-left"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_size(JS::Realm& realm, StringView value)
{
    return set_property(realm, "size"_fly_string, value, ""sv);
}

String CSSPageDescriptors::size() const
{
    return get_property_value("size"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_page_orientation(JS::Realm& realm, StringView value)
{
    return set_property(realm, "page-orientation"_fly_string, value, ""sv);
}

String CSSPageDescriptors::page_orientation() const
{
    return get_property_value("page-orientation"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_marks(JS::Realm& realm, StringView value)
{
    return set_property(realm, "marks"_fly_string, value, ""sv);
}

String CSSPageDescriptors::marks() const
{
    return get_property_value("marks"_fly_string);
}

WebIDL::ExceptionOr<void> CSSPageDescriptors::set_bleed(JS::Realm& realm, StringView value)
{
    return set_property(realm, "bleed"_fly_string, value, ""sv);
}

String CSSPageDescriptors::bleed() const
{
    return get_property_value("bleed"_fly_string);
}

}
