/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RedBlackTree.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Bindings/KeyframeEffectPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::Animations {

using EasingValue = Variant<String, NonnullRefPtr<CSS::CSSStyleValue const>>;

// https://www.w3.org/TR/web-animations-1/#the-keyframeeffectoptions-dictionary
struct KeyframeEffectOptions : public EffectTiming {
    Bindings::CompositeOperation composite { Bindings::CompositeOperation::Replace };
    Optional<String> pseudo_element {};
};

// https://www.w3.org/TR/web-animations-1/#dictdef-basepropertyindexedkeyframe
// Note: This is an intermediate structure used only when parsing Keyframes provided by the caller in a slightly
//       different format. It is converted to BaseKeyframe, which is why it doesn't need to store the parsed properties
struct BasePropertyIndexedKeyframe {
    Variant<Optional<double>, Vector<Optional<double>>> offset { Vector<Optional<double>> {} };
    Variant<EasingValue, Vector<EasingValue>> easing { Vector<EasingValue> {} };
    Variant<Bindings::CompositeOperationOrAuto, Vector<Bindings::CompositeOperationOrAuto>> composite { Vector<Bindings::CompositeOperationOrAuto> {} };

    HashMap<String, Vector<String>> properties {};
};

// https://www.w3.org/TR/web-animations-1/#dictdef-basekeyframe
struct BaseKeyframe {
    using UnparsedProperties = HashMap<String, String>;
    using ParsedProperties = HashMap<CSS::PropertyID, NonnullRefPtr<CSS::CSSStyleValue const>>;

    Optional<double> offset {};
    EasingValue easing { "linear"_string };
    Bindings::CompositeOperationOrAuto composite { Bindings::CompositeOperationOrAuto::Auto };

    Optional<double> computed_offset {};

    Variant<UnparsedProperties, ParsedProperties> properties { UnparsedProperties {} };

    UnparsedProperties& unparsed_properties() { return properties.get<UnparsedProperties>(); }
    ParsedProperties& parsed_properties() { return properties.get<ParsedProperties>(); }
};

// https://www.w3.org/TR/web-animations-1/#the-keyframeeffect-interface
class KeyframeEffect : public AnimationEffect {
    WEB_PLATFORM_OBJECT(KeyframeEffect, AnimationEffect);
    JS_DECLARE_ALLOCATOR(KeyframeEffect);

public:
    constexpr static double AnimationKeyFrameKeyScaleFactor = 1000.0; // 0..100000

    struct KeyFrameSet : public RefCounted<KeyFrameSet> {
        struct UseInitial { };
        struct ResolvedKeyFrame {
            // These CSSStyleValue properties can be unresolved, as they may be generated from a @keyframes rule, well
            // before they are applied to an element
            HashMap<CSS::PropertyID, Variant<UseInitial, NonnullRefPtr<CSS::CSSStyleValue const>>> properties {};
        };
        RedBlackTree<u64, ResolvedKeyFrame> keyframes_by_key;
    };
    static void generate_initial_and_final_frames(RefPtr<KeyFrameSet>, HashTable<CSS::PropertyID> const& animated_properties);

    static int composite_order(JS::NonnullGCPtr<KeyframeEffect>, JS::NonnullGCPtr<KeyframeEffect>);

    static JS::NonnullGCPtr<KeyframeEffect> create(JS::Realm&);

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<KeyframeEffect>> construct_impl(
        JS::Realm&,
        JS::Handle<DOM::Element> const& target,
        Optional<JS::Handle<JS::Object>> const& keyframes,
        Variant<double, KeyframeEffectOptions> options = KeyframeEffectOptions {});

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<KeyframeEffect>> construct_impl(JS::Realm&, JS::NonnullGCPtr<KeyframeEffect> source);

    DOM::Element* target() const override { return m_target_element; }
    void set_target(DOM::Element* target);

    // JS bindings
    Optional<String> pseudo_element() const;
    WebIDL::ExceptionOr<void> set_pseudo_element(Optional<String>);

    Optional<CSS::Selector::PseudoElement::Type> pseudo_element_type() const;
    void set_pseudo_element(Optional<CSS::Selector::PseudoElement> pseudo_element) { m_target_pseudo_selector = pseudo_element; }

    Bindings::CompositeOperation composite() const { return m_composite; }
    void set_composite(Bindings::CompositeOperation value) { m_composite = value; }

    WebIDL::ExceptionOr<JS::MarkedVector<JS::Object*>> get_keyframes();
    WebIDL::ExceptionOr<void> set_keyframes(Optional<JS::Handle<JS::Object>> const&);

    KeyFrameSet const* key_frame_set() { return m_key_frame_set; }
    void set_key_frame_set(RefPtr<KeyFrameSet const> key_frame_set) { m_key_frame_set = key_frame_set; }

    virtual bool is_keyframe_effect() const override { return true; }

    virtual void update_style_properties() override;

    Optional<CSS::AnimationPlayState> last_css_animation_play_state() const { return m_last_css_animation_play_state; }
    void set_last_css_animation_play_state(CSS::AnimationPlayState state) { m_last_css_animation_play_state = state; }

private:
    KeyframeEffect(JS::Realm&);
    virtual ~KeyframeEffect() override = default;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://www.w3.org/TR/web-animations-1/#effect-target-target-element
    JS::GCPtr<DOM::Element> m_target_element {};

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-pseudoelement
    Optional<CSS::Selector::PseudoElement> m_target_pseudo_selector {};

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-composite
    Bindings::CompositeOperation m_composite { Bindings::CompositeOperation::Replace };

    // https://www.w3.org/TR/web-animations-1/#keyframe
    Vector<BaseKeyframe> m_keyframes {};

    // A cached version of m_keyframes suitable for returning from get_keyframes()
    Vector<JS::NonnullGCPtr<JS::Object>> m_keyframe_objects {};

    RefPtr<KeyFrameSet const> m_key_frame_set {};

    Optional<CSS::AnimationPlayState> m_last_css_animation_play_state;
};

}
