/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedEnumeration
class SVGAnimatedEnumeration final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SVGAnimatedEnumeration, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedEnumeration);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedEnumeration> create(u16 value);
    virtual ~SVGAnimatedEnumeration() override;

    u16 base_val() const { return base_or_anim_value(); }
    WebIDL::ExceptionOr<void> set_base_val(u16);

    u16 anim_val() const { return base_or_anim_value(); }

private:
    SVGAnimatedEnumeration(u16 value);

    u16 base_or_anim_value() const;

    u16 m_value;
};

}
