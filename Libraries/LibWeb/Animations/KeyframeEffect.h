/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RedBlackTree.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Bindings/KeyframeEffect.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Compositor/VisualAnimation.h>

namespace Web::Animations {

using EasingValue = Variant<Utf16String, CSS::EasingFunction>;
using CompositeOperation = Bindings::CompositeOperation;
using CompositeOperationOrAuto = Bindings::CompositeOperationOrAuto;

Bindings::CompositeOperation css_animation_composition_to_bindings_composite_operation(CSS::AnimationComposition composition);
Bindings::CompositeOperationOrAuto css_animation_composition_to_bindings_composite_operation_or_auto(CSS::AnimationComposition composition);

// https://www.w3.org/TR/web-animations-1/#dictdef-basepropertyindexedkeyframe
// Note: This is an intermediate structure used only when parsing Keyframes provided by the caller in a slightly
//       different format. It is converted to BaseKeyframe, which is why it doesn't need to store the parsed properties
struct BasePropertyIndexedKeyframe {
    Variant<Optional<double>, Vector<Optional<double>>> offset { Vector<Optional<double>> {} };
    Variant<EasingValue, Vector<EasingValue>> easing { Vector<EasingValue> {} };
    Variant<Bindings::CompositeOperationOrAuto, Vector<Bindings::CompositeOperationOrAuto>> composite { Vector<Bindings::CompositeOperationOrAuto> {} };

    HashMap<Utf16FlyString, Vector<Utf16String>> properties {};
};

CompositeOperation css_animation_composition_to_composite_operation(CSS::AnimationComposition composition);
CompositeOperationOrAuto css_animation_composition_to_composite_operation_or_auto(CSS::AnimationComposition composition);
StringView composite_operation_or_auto_to_string(CompositeOperationOrAuto);

// https://www.w3.org/TR/web-animations-1/#dictdef-basekeyframe
struct BaseKeyframe {
    using UnparsedProperties = HashMap<Utf16FlyString, Utf16String>;
    using ParsedProperties = HashMap<CSS::PropertyNameAndID, CSS::RustStyleValueHandle>;

    Optional<double> offset {};
    EasingValue easing { "linear"_utf16 };
    Bindings::CompositeOperationOrAuto composite { Bindings::CompositeOperationOrAuto::Auto };

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
        struct StyleSheetResourceContext {
            String base_url;
            bool origin_clean { false };
        };
        struct UseInitial { };
        struct ResolvedKeyFrame {
            // These style values can be unresolved, as they may be generated from a @keyframes rule, well
            // before they are applied to an element
            HashMap<CSS::PropertyNameAndID, Variant<UseInitial, CSS::RustStyleValueHandle>> properties {};
            CompositeOperationOrAuto composite { CompositeOperationOrAuto::Auto };
            Variant<Empty, CSS::EasingFunction, CSS::RustStyleValueHandle> easing {};
        };
        RedBlackTree<u64, ResolvedKeyFrame> keyframes_by_key;
        Optional<StyleSheetResourceContext> style_sheet_resource_context;
    };
    static void generate_initial_and_final_frames(RefPtr<KeyFrameSet>, HashTable<CSS::PropertyNameAndID> const& animated_properties);

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

    virtual GC::Ptr<DOM::Element> target() const override { return m_target_element; }
    void set_target(GC::Ptr<DOM::Element> target);

    // JS bindings
    Optional<Utf16String> pseudo_element() const;
    WebIDL::ExceptionOr<void> set_pseudo_element(Optional<Utf16String>);

    Optional<DOM::AbstractElement> target_abstract_element() const;
    enum class InvalidateEffect {
        No,
        Yes,
    };
    void set_target(DOM::AbstractElement, InvalidateEffect = InvalidateEffect::Yes);

    Optional<CSS::PseudoElement> pseudo_element_type() const;
    void set_pseudo_element(Optional<CSS::Selector::PseudoElementSelector> pseudo_element) { m_target_pseudo_selector = pseudo_element; }

    Bindings::CompositeOperation composite_for_bindings() const;
    void set_composite_for_bindings(Bindings::CompositeOperation value) { set_composite(value); }
    Bindings::CompositeOperation composite() const { return m_composite; }
    void set_composite(Bindings::CompositeOperation value);

    Vector<BaseKeyframe> const& keyframes() const { return m_keyframes; }
    void set_keyframes(Vector<BaseKeyframe>);
    WebIDL::ExceptionOr<void> set_keyframes_from_js(JS::Realm&, GC::Ptr<JS::Object>);
    WebIDL::ExceptionOr<GC::RootVector<GC::Ref<JS::Object>>> get_keyframes(JS::Object& relevant_global_object);

    KeyFrameSet const* key_frame_set() { return m_key_frame_set; }
    void set_key_frame_set(RefPtr<KeyFrameSet const>);

    virtual bool is_keyframe_effect() const override { return true; }

    bool can_skip_per_frame_style_update() const;
    void clear_per_frame_style_update_cache() { m_can_skip_per_frame_style_update_cache.clear(); }
    bool can_skip_per_frame_animation_tick() const;
    bool is_compositor_driven() const { return m_is_compositor_driven; }
    void set_is_compositor_driven(bool value)
    {
        if (m_is_compositor_driven == value)
            return;
        m_is_compositor_driven = value;
        m_can_skip_per_frame_style_update_cache.clear();
    }
    bool is_compositor_replaced() const { return m_is_compositor_replaced; }
    void set_is_compositor_replaced(bool value)
    {
        if (m_is_compositor_replaced == value)
            return;
        m_is_compositor_replaced = value;
        m_can_skip_per_frame_style_update_cache.clear();
    }
    Vector<Compositor::VisualAnimation> const& retained_compositor_animations() const { return m_retained_compositor_animations; }
    void set_retained_compositor_animations(Vector<Compositor::VisualAnimation> animations) { m_retained_compositor_animations = move(animations); }
    void clear_retained_compositor_animations() { m_retained_compositor_animations.clear(); }
    void set_is_offscreen_throttled(bool value)
    {
        if (m_is_offscreen_throttled == value)
            return;
        m_is_offscreen_throttled = value;
        m_can_skip_per_frame_style_update_cache.clear();
    }
    bool is_offscreen_throttled() const { return m_is_offscreen_throttled; }
    void set_is_observation_relevant_compositor_animation(bool value) { m_is_observation_relevant_compositor_animation = value; }
    bool is_observation_relevant_compositor_animation() const { return m_is_observation_relevant_compositor_animation; }
    void request_observation_sample();
    bool request_element_scoped_observation_sample(u64 task_generation)
    {
        if (m_last_element_scoped_observation_sample_task_generation == task_generation)
            return false;
        m_last_element_scoped_observation_sample_task_generation = task_generation;
        request_observation_sample();
        return true;
    }
    bool observation_sample_requested() const { return m_needs_observation_sample; }
    bool consume_observation_sample_request() { return exchange(m_needs_observation_sample, false); }
    bool per_frame_animation_tick_was_skipped() const { return m_per_frame_animation_tick_was_skipped; }
    void note_per_frame_animation_tick_was_skipped() { m_per_frame_animation_tick_was_skipped = true; }
    void clear_per_frame_animation_tick_was_skipped() { m_per_frame_animation_tick_was_skipped = false; }
    struct CompositorKeyframeValueCache {
        KeyFrameSet const* key_frame_set { nullptr };
        u64 target_style_generation { 0 };
        u64 style_environment_version { 0 };
        float reference_width { 0 };
        float reference_height { 0 };
        float device_pixels_per_css_pixel { 0 };
        bool is_valid { false };
        Vector<Optional<Compositor::VisualAnimationValue>> values;
    };
    Optional<CompositorKeyframeValueCache>& compositor_keyframe_value_cache(Compositor::VisualAnimation::TargetKind target_kind)
    {
        return target_kind == Compositor::VisualAnimation::TargetKind::Opacity
            ? m_compositor_opacity_keyframe_value_cache
            : m_compositor_transform_keyframe_value_cache;
    }
    virtual void update_computed_properties(AnimationUpdateContext&) override;
    void update_computed_properties_for_style(AnimationUpdateContext&, DOM::AbstractElement);

private:
    friend class Animation;

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

    // A cached version of m_keyframes suitable for returning from get_keyframes()
    Vector<GC::Ref<JS::Object>> m_keyframe_objects_cache {};

    RefPtr<KeyFrameSet const> m_key_frame_set {};
    Optional<CompositorKeyframeValueCache> m_compositor_opacity_keyframe_value_cache;
    Optional<CompositorKeyframeValueCache> m_compositor_transform_keyframe_value_cache;
    Vector<Compositor::VisualAnimation> m_retained_compositor_animations;
    bool m_is_compositor_driven { false };
    bool m_is_compositor_replaced { false };
    bool m_is_offscreen_throttled { false };
    bool m_is_observation_relevant_compositor_animation { false };
    bool m_needs_observation_sample { false };
    struct CanSkipPerFrameStyleUpdateCache {
        u64 target_style_generation { 0 };
        u64 target_subtree_style_generation { 0 };
        bool target_is_connected { false };
        Layout::Node const* layout_node { nullptr };
        bool result { false };
    };
    mutable Optional<CanSkipPerFrameStyleUpdateCache> m_can_skip_per_frame_style_update_cache;
    Optional<u64> m_last_element_scoped_observation_sample_task_generation;
    bool m_per_frame_animation_tick_was_skipped { false };
};

WebIDL::ExceptionOr<Vector<BaseKeyframe>> process_keyframes(JS::Realm&, GC::Ptr<JS::Object>);
WebIDL::ExceptionOr<KeyframeEffect::Options> keyframe_effect_options_from_bindings(Bindings::KeyframeEffectOptions const&);

}
