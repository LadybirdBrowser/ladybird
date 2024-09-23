/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-animations-2/#cssanimation
class CSSAnimation : public Animations::Animation {
    WEB_PLATFORM_OBJECT(CSSAnimation, Animations::Animation);
    JS_DECLARE_ALLOCATOR(CSSAnimation);

public:
    static JS::NonnullGCPtr<CSSAnimation> create(JS::Realm&);

    FlyString const& animation_name() const { return id(); }

    virtual Animations::AnimationClass animation_class() const override;
    virtual Optional<int> class_specific_composite_order(JS::NonnullGCPtr<Animations::Animation> other) const override;

private:
    explicit CSSAnimation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_css_animation() const override { return true; }
};

}
