/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimationElement.h>

namespace Web::SVG {

class SVGSetElement final : public SVGAnimationElement {
    WEB_WRAPPABLE(SVGSetElement, SVGAnimationElement);
    GC_DECLARE_ALLOCATOR(SVGSetElement);

private:
    SVGSetElement(DOM::Document&, DOM::QualifiedName);

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const override;
};

}
