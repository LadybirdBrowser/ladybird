/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustCSSDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(RustCSSDescriptors);

GC::Ref<RustCSSDescriptors> RustCSSDescriptors::create(AtRuleID at_rule, RustDescriptorBlock descriptors)
{
    return GC::Heap::the().allocate<RustCSSDescriptors>(at_rule, move(descriptors));
}

RustCSSDescriptors::RustCSSDescriptors(AtRuleID at_rule, RustDescriptorBlock descriptors)
    : CSSDescriptors(at_rule, move(descriptors))
    , m_at_rule(at_rule)
{
}

WebIDL::ExceptionOr<void> RustCSSDescriptors::set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority)
{
    TRY(Base::set_property(property, value, priority));

    if (m_at_rule == AtRuleID::FontFace) {
        if (auto* font_face_rule = as_if<CSSFontFaceRule>(parent_rule().ptr()))
            font_face_rule->handle_descriptor_change(property);
    }

    return {};
}

}
