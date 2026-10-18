/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Path.h>
#include <LibWeb/SVG/SVGAnimationElement.h>

namespace Web::SVG {

class SVGAnimateMotionElement final : public SVGAnimationElement {
    WEB_WRAPPABLE(SVGAnimateMotionElement, SVGAnimationElement);
    GC_DECLARE_ALLOCATOR(SVGAnimateMotionElement);

public:
    void schedule_referenced_motion_path_update();
    void update_referenced_motion_path();

private:
    SVGAnimateMotionElement(DOM::Document&, DOM::QualifiedName);

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<CSS::PropertyID> target_property() const override;
    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const override;
    static Vector<Animations::BaseKeyframe> keyframes_along_motion_path(Gfx::Path const&, CSS::PropertyID);

    Optional<Gfx::Path> m_referenced_motion_path;
};

}
