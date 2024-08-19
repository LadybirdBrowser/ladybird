/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Animations/KeyframeEffect.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#dictdef-keyframeanimationoptions
struct KeyframeAnimationOptions : public KeyframeEffectOptions {
    FlyString id { ""_fly_string };
    Optional<GC::Ptr<AnimationTimeline>> timeline;
};

// https://www.w3.org/TR/web-animations-1/#dictdef-getanimationsoptions
struct GetAnimationsOptions {
    bool subtree { false };
};

// https://www.w3.org/TR/web-animations-1/#animatable
class Animatable {
public:
    virtual ~Animatable() = default;

    WebIDL::ExceptionOr<GC::Ref<Animation>> animate(Optional<GC::Handle<JS::Object>> keyframes, Variant<Empty, double, KeyframeAnimationOptions> options = {});
    Vector<GC::Ref<Animation>> get_animations(GetAnimationsOptions options = {});

    void associate_with_animation(GC::Ref<Animation>);
    void disassociate_with_animation(GC::Ref<Animation>);

    GC::Ptr<CSS::CSSStyleDeclaration const> cached_animation_name_source(Optional<CSS::Selector::PseudoElement::Type>) const;
    void set_cached_animation_name_source(GC::Ptr<CSS::CSSStyleDeclaration const> value, Optional<CSS::Selector::PseudoElement::Type>);

    GC::Ptr<Animations::Animation> cached_animation_name_animation(Optional<CSS::Selector::PseudoElement::Type>) const;
    void set_cached_animation_name_animation(GC::Ptr<Animations::Animation> value, Optional<CSS::Selector::PseudoElement::Type>);

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    Vector<GC::Ref<Animation>> m_associated_animations;
    bool m_is_sorted_by_composite_order { true };

    Array<GC::Ptr<CSS::CSSStyleDeclaration const>, to_underlying(CSS::Selector::PseudoElement::Type::KnownPseudoElementCount) + 1> m_cached_animation_name_source;
    Array<GC::Ptr<Animations::Animation>, to_underlying(CSS::Selector::PseudoElement::Type::KnownPseudoElementCount) + 1> m_cached_animation_name_animation;
};

}
