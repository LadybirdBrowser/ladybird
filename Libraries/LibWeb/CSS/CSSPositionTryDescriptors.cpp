/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSPositionTryDescriptors.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPositionTryDescriptors);

GC::Ref<CSSPositionTryDescriptors> CSSPositionTryDescriptors::create(RustDeclarationBlock declarations)
{
    return GC::Heap::the().allocate<CSSPositionTryDescriptors>(move(declarations));
}

CSSPositionTryDescriptors::CSSPositionTryDescriptors(RustDeclarationBlock declarations)
    : CSSStyleProperties(Computed::No, Readonly::No, move(declarations), OptionalNone {})
{
}

// https://drafts.csswg.org/css-anchor-position-1/#accepted-position-try-properties
static bool is_accepted_position_try_property(PropertyID property_id)
{
    switch (property_id) {
#define __ENUMERATE(name, property_name, property_id) case PropertyID::property_id:
        ENUMERATE_CSS_POSITION_TRY_DESCRIPTORS(__ENUMERATE)
#undef __ENUMERATE
        return true;
    default:
        return false;
    }
}

WebIDL::ExceptionOr<void> CSSPositionTryDescriptors::set_property(Utf16FlyString const& property_name, Utf16View css_text, Utf16View priority)
{
    // https://drafts.csswg.org/css-anchor-position-1/#fallback-rule
    // The @position-try rule only accepts the following accepted @position-try properties [...]
    // It is invalid to use !important on the properties in the <declaration-list>.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value() || property->is_custom_property() || !is_accepted_position_try_property(property->id()) || !priority.is_empty())
        return {};
    return Base::set_property(property_name, css_text, priority);
}

#define __ENUMERATE(name, property_name, property_id)                                   \
    WebIDL::ExceptionOr<void> CSSPositionTryDescriptors::set_##name(Utf16View css_text) \
    {                                                                                   \
        return set_property(property_name##_utf16_fly_string, css_text, u""sv);         \
    }                                                                                   \
    Utf16String CSSPositionTryDescriptors::name() const                                 \
    {                                                                                   \
        return get_property_value(property_name##_utf16_fly_string);                    \
    }
ENUMERATE_CSS_POSITION_TRY_DESCRIPTORS(__ENUMERATE)
#undef __ENUMERATE

}
