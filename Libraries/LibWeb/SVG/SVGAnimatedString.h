/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedString
class SVGAnimatedString final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedString, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(SVGAnimatedString);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SVGAnimatedString> create(JS::Realm&, JS::NonnullGCPtr<SVGElement> element, FlyString reflected_attribute, Optional<FlyString> deprecated_reflected_attribute = {}, Optional<FlyString> initial_value = {});
    virtual ~SVGAnimatedString() override;

    String base_val() const;
    void set_base_val(String const& base_val);

private:
    SVGAnimatedString(JS::Realm&, JS::NonnullGCPtr<SVGElement> element, FlyString reflected_attribute, Optional<FlyString> deprecated_reflected_attribute, Optional<FlyString> initial_value);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<SVGElement> m_element;
    FlyString m_reflected_attribute;
    Optional<FlyString> m_deprecated_reflected_attribute;
    Optional<FlyString> m_initial_value;
};

}
