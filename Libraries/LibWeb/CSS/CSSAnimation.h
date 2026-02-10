/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-animations-2/#cssanimation
class CSSAnimation : public Animations::Animation {
    WEB_PLATFORM_OBJECT(CSSAnimation, Animations::Animation);
    GC_DECLARE_ALLOCATOR(CSSAnimation);

public:
    static GC::Ref<CSSAnimation> create(JS::Realm&);

    FlyString const& animation_name() const { return m_animation_name; }
    void set_animation_name(FlyString const& animation_name) { m_animation_name = animation_name; }

    virtual Animations::AnimationClass animation_class() const override;
    virtual int class_specific_composite_order(GC::Ref<Animations::Animation> other) const override;

    void apply_css_properties(ComputedProperties::AnimationProperties const&);

    virtual void set_timeline_for_bindings(GC::Ptr<Animations::AnimationTimeline> timeline) override;

private:
    explicit CSSAnimation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_css_animation() const override { return true; }

    // https://drafts.csswg.org/css-animations-2/#dom-cssanimation-animationname
    FlyString m_animation_name;

    HashTable<CSS::PropertyID> m_ignored_css_properties;
};

}
