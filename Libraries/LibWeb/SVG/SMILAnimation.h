/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

class SMILAnimation final : public Animations::Animation {
    WEB_NON_IDL_WRAPPABLE(SMILAnimation, Animations::Animation);
    GC_DECLARE_ALLOCATOR(SMILAnimation);

public:
    static GC::Ref<SMILAnimation> create(SVGAnimationElement&, SMILTimeContainer&);

    SVGAnimationElement& animation_element() const { return m_animation_element; }

    virtual void cancel(ShouldInvalidate = ShouldInvalidate::Yes) override;

    virtual bool is_smil_animation() const override { return true; }
    virtual Animations::AnimationClass animation_class() const override { return Animations::AnimationClass::SMILAnimation; }
    virtual int class_specific_composite_order(GC::Ref<Animations::Animation> other) const override;

private:
    SMILAnimation(HTML::EnvironmentSettingsObject&, SVGAnimationElement&, SMILTimeContainer&);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<SVGAnimationElement> m_animation_element;
    GC::Ref<SMILTimeContainer> m_time_container;
};

}
