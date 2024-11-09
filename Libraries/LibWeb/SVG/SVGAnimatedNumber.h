/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/types.html#InterfaceSVGAnimatedNumber
class SVGAnimatedNumber final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedNumber, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(SVGAnimatedNumber);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SVGAnimatedNumber> create(JS::Realm&, float base_val, float anim_val);
    virtual ~SVGAnimatedNumber() override;

    float base_val() const { return m_base_val; }
    float anim_val() const { return m_anim_val; }

    void set_base_val(float base_val) { m_base_val = base_val; }
    void set_anim_val(float anim_val) { m_anim_val = anim_val; }

private:
    SVGAnimatedNumber(JS::Realm&, float base_val, float anim_val);

    virtual void initialize(JS::Realm&) override;

    float m_base_val;
    float m_anim_val;
};

}
