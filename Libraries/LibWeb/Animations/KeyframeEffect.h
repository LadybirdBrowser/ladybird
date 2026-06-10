/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RedBlackTree.h>
#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Bindings/KeyframeEffect.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::Animations {

using EasingValue = Variant<String, CSS::EasingFunction>;

using CompositeOperation = Bindings::CompositeOperation;
using CompositeOperationOrAuto = Bindings::CompositeOperationOrAuto;

CompositeOperation css_animation_composition_to_composite_operation(CSS::AnimationComposition composition);
CompositeOperationOrAuto css_animation_composition_to_composite_operation_or_auto(CSS::AnimationComposition composition);
StringView composite_operation_or_auto_to_string(CompositeOperationOrAuto);

// https://www.w3.org/TR/web-animations-1/#dictdef-basekeyframe
struct BaseKeyframe {
    using UnparsedProperties = HashMap<String, String>;
    using ParsedProperties = HashMap<CSS::PropertyID, NonnullRefPtr<CSS::StyleValue const>>;

    Optional<double> offset {};
    EasingValue easing { "linear"_string };
    CompositeOperationOrAuto composite { CompositeOperationOrAuto::Auto };

    Optional<double> computed_offset {};

    Variant<UnparsedProperties, ParsedProperties> properties { UnparsedProperties {} };

    UnparsedProperties& unparsed_properties() { return properties.get<UnparsedProperties>(); }
    UnparsedProperties const& unparsed_properties() const { return properties.get<UnparsedProperties>(); }
    ParsedProperties& parsed_properties() { return properties.get<ParsedProperties>(); }
    ParsedProperties const& parsed_properties() const { return properties.get<ParsedProperties>(); }
};

void compute_missing_keyframe_offsets(Vector<BaseKeyframe>&);

// https://www.w3.org/TR/web-animations-1/#the-keyframeeffect-interface
class KeyframeEffect final : public AnimationEffect {
    WEB_WRAPPABLE(KeyframeEffect, AnimationEffect);
    GC_DECLARE_ALLOCATOR(KeyframeEffect);

public:
    constexpr static double AnimationKeyFrameKeyScaleFactor = 1000.0; // 0..100000

    struct Options {
        OptionalEffectTiming timing {};
        CompositeOperation composite { CompositeOperation::Replace };
        Optional<CSS::Selector::PseudoElementSelector> pseudo_element;
    };

    struct KeyFrameSet : public RefCounted<KeyFrameSet> {
        struct UseInitial { };
        struct ResolvedKeyFrame {
            // These StyleValue properties can be unresolved, as they may be generated from a @keyframes rule, well
            // before they are applied to an element
            HashMap<CSS::PropertyID, Variant<UseInitial, NonnullRefPtr<CSS::StyleValue const>>> properties {};
            CompositeOperationOrAuto composite { CompositeOperationOrAuto::Auto };
            Variant<Empty, CSS::EasingFunction, NonnullRefPtr<CSS::StyleValue const>> easing {};
        };
        RedBlackTree<u64, ResolvedKeyFrame> keyframes_by_key;
    };
    static void generate_initial_and_final_frames(RefPtr<KeyFrameSet>, HashTable<CSS::PropertyID> const& animated_properties);

    static int composite_order(GC::Ref<KeyframeEffect>, GC::Ref<KeyframeEffect>);

    static GC::Ref<KeyframeEffect> create();

    static WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> create_from_processed_keyframes(
        GC::Ptr<DOM::Element> target,
        Vector<BaseKeyframe> keyframes,
        Variant<double, Options> options);

    static WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> construct_impl(
        JS::Realm&,
        GC::Ptr<DOM::Element> target,
        GC::Ptr<JS::Object> keyframes,
        Variant<double, Bindings::KeyframeEffectOptions> const& options);
    static WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> construct_impl(GC::Ref<KeyframeEffect> source);
    static WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> create_copy(GC::Ref<KeyframeEffect> source);

    DOM::Element* target() const override { return m_target_element; }
    void set_target(DOM::Element* target);

    // JS bindings
    Optional<String> pseudo_element() const;
    WebIDL::ExceptionOr<void> set_pseudo_element(Optional<String>);

    Optional<DOM::AbstractElement> target_abstract_element() const;
    void set_target(DOM::AbstractElement);

    Optional<CSS::PseudoElement> pseudo_element_type() const;
    void set_pseudo_element(Optional<CSS::Selector::PseudoElementSelector> pseudo_element) { m_target_pseudo_selector = pseudo_element; }

    CompositeOperation composite() const { return m_composite; }
    void set_composite(CompositeOperation value);

    Vector<BaseKeyframe> const& keyframes() const { return m_keyframes; }
    void set_keyframes(Vector<BaseKeyframe>);
    WebIDL::ExceptionOr<void> set_keyframes_from_js(JS::Realm&, GC::Ptr<JS::Object>);
    WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> get_keyframes(JS::Realm&) const;

    KeyFrameSet const* key_frame_set() { return m_key_frame_set; }
    void set_key_frame_set(RefPtr<KeyFrameSet const> key_frame_set) { m_key_frame_set = key_frame_set; }

    virtual bool is_keyframe_effect() const override { return true; }

    virtual void update_computed_properties(AnimationUpdateContext&) override;

private:
    KeyframeEffect();
    virtual ~KeyframeEffect() override = default;

    void invalidate_effect();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://www.w3.org/TR/web-animations-1/#effect-target-target-element
    GC::Ptr<DOM::Element> m_target_element {};

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-pseudoelement
    Optional<CSS::Selector::PseudoElementSelector> m_target_pseudo_selector {};

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-composite
    CompositeOperation m_composite { CompositeOperation::Replace };

    // https://www.w3.org/TR/web-animations-1/#keyframe
    Vector<BaseKeyframe> m_keyframes {};

    RefPtr<KeyFrameSet const> m_key_frame_set {};
};

WebIDL::ExceptionOr<Vector<BaseKeyframe>> process_keyframes(JS::Realm&, GC::Ptr<JS::Object>);
WebIDL::ExceptionOr<KeyframeEffect::Options> keyframe_effect_options_from_bindings(Bindings::KeyframeEffectOptions const&);

}
