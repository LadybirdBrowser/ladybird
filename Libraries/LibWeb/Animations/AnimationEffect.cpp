/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Bindings/AnimationEffect.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationEffect);

AnimationUpdateContext::ElementData::ElementData() = default;

AnimationUpdateContext::ElementData::ElementData(CSS::InstalledStyle style_before_update, RefPtr<CSS::ComputedStyleWorkingSet> target_style)
    : style_before_update(move(style_before_update))
    , target_style(move(target_style))
{
}

AnimationUpdateContext::ElementData::ElementData(ElementData&&) = default;

AnimationUpdateContext::ElementData& AnimationUpdateContext::ElementData::operator=(ElementData&&) = default;

AnimationUpdateContext::ElementData::~ElementData() = default;

AnimationUpdateContext::AnimationUpdateContext() = default;

Bindings::FillMode css_fill_mode_to_bindings_fill_mode(CSS::AnimationFillMode mode)
{
    switch (mode) {
    case CSS::AnimationFillMode::Backwards:
        return Bindings::FillMode::Backwards;
    case CSS::AnimationFillMode::Both:
        return Bindings::FillMode::Both;
    case CSS::AnimationFillMode::Forwards:
        return Bindings::FillMode::Forwards;
    case CSS::AnimationFillMode::None:
        return Bindings::FillMode::None;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::PlaybackDirection css_animation_direction_to_bindings_playback_direction(CSS::AnimationDirection direction)
{
    switch (direction) {
    case CSS::AnimationDirection::Alternate:
        return Bindings::PlaybackDirection::Alternate;
    case CSS::AnimationDirection::AlternateReverse:
        return Bindings::PlaybackDirection::AlternateReverse;
    case CSS::AnimationDirection::Normal:
        return Bindings::PlaybackDirection::Normal;
    case CSS::AnimationDirection::Reverse:
        return Bindings::PlaybackDirection::Reverse;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::OptionalEffectTiming to_optional_effect_timing(Bindings::EffectTiming const& effect_timing)
{
    return {
        .delay = effect_timing.delay,
        .direction = effect_timing.direction,
        .duration = effect_timing.duration.visit(
            [](double const& value) -> Variant<double, Utf16String> { return value; },
            [](Utf16String const& value) -> Variant<double, Utf16String> { return value; },
            // NB: We check that this isn't the case in the caller
            [](GC::Ref<CSS::CSSNumericValue>) -> Variant<double, Utf16String> { VERIFY_NOT_REACHED(); }),
        .easing = effect_timing.easing,
        .end_delay = effect_timing.end_delay,
        .fill = effect_timing.fill,
        .iteration_start = effect_timing.iteration_start,
        .iterations = effect_timing.iterations,
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-gettiming
Bindings::EffectTiming AnimationEffect::get_timing() const
{
    update_style_if_needed();

    // 1. Returns the specified timing properties for this animation effect.
    return {
        .delay = m_specified_start_delay,
        .direction = m_playback_direction,
        .duration = m_specified_iteration_duration,
        .easing = m_timing_function.to_utf16_string(),
        .end_delay = m_specified_end_delay,
        .fill = m_fill_mode,
        .iteration_start = m_iteration_start,
        .iterations = m_iteration_count,
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-getcomputedtiming
// https://drafts.csswg.org/web-animations-2/#dom-animationeffect-getcomputedtiming
Bindings::ComputedEffectTiming AnimationEffect::get_computed_timing() const
{
    update_style_if_needed();

    if (m_associated_animation) {
        // NB: Observing the current time can ask for the phase of this effect, which must not read the time it observed
        //     last.
        auto observed_local_time = m_associated_animation->current_time_for_observation();
        m_has_local_time_override_for_observation = true;
        m_local_time_override_for_observation = observed_local_time;
    }
    ScopeGuard clear_local_time_override = [&] {
        m_has_local_time_override_for_observation = false;
    };

    // 1. Returns the calculated timing properties for this animation effect.

    // Note: Although some of the attributes of the object returned by getTiming() and getComputedTiming() are common,
    //       their values may differ in the following ways:

    //     - duration: while getTiming() may return the string auto, getComputedTiming() must return a number
    //       corresponding to the calculated value of the iteration duration as defined in the description of the
    //       duration member of the EffectTiming interface.
    //
    //       If duration is the string auto, this attribute will return the current calculated value of the intrinsic
    //       iteration duration, which may be a expressed as a double representing the duration in milliseconds or a
    //       percentage when the effect is associated with a progress-based timeline.
    auto duration = m_iteration_duration.as_css_numberish();

    //     - fill: likewise, while getTiming() may return the string auto, getComputedTiming() must return the specific
    //       FillMode used for timing calculations as defined in the description of the fill member of the EffectTiming
    //       interface.
    //
    //       In this level of the specification, that simply means that an auto value is replaced by the none FillMode.
    auto fill = m_fill_mode == Bindings::FillMode::Auto ? Bindings::FillMode::None : m_fill_mode;

    Bindings::ComputedEffectTiming computed_timing {};
    computed_timing.delay = m_specified_start_delay;
    computed_timing.end_delay = m_specified_end_delay;
    computed_timing.fill = fill;
    computed_timing.iteration_start = m_iteration_start;
    computed_timing.iterations = m_iteration_count;
    computed_timing.duration = duration;
    computed_timing.direction = m_playback_direction;
    computed_timing.easing = m_timing_function.to_utf16_string();
    auto local_time = this->local_time();
    auto timing = resolve_timing_at(local_time, m_fill_mode, WithTransformedProgress::Yes);
    computed_timing.active_duration = timing.active_duration.as_css_numberish();
    computed_timing.current_iteration = from_ffi(timing.current_iteration);
    computed_timing.end_time = timing.end_time.as_css_numberish();
    computed_timing.local_time = NullableCSSNumberish::from_optional_css_numberish_time(local_time);
    computed_timing.progress = from_ffi(timing.transformed_progress);
    return computed_timing;
}

GC::Ptr<AnimationTimeline> AnimationEffect::associated_timeline() const
{
    if (!m_associated_animation)
        return {};

    return m_associated_animation->timeline();
}

Optional<TimeValue> AnimationEffect::timeline_duration() const
{
    auto timeline = associated_timeline();
    if (!timeline)
        return {};

    // AD-HOC: The duration of a progress-based timeline, as an animation attached to a range of it sees it, is the
    //         length of its attachment range. This matches WebKit.
    //         An attachment range whose end comes before its start is empty.
    if (auto interval = m_associated_animation->attachment_interval(); interval.has_value())
        return max(interval->end - interval->start, TimeValue { TimeValue::Type::Percentage, 0 });

    return timeline->duration();
}

static_assert(sizeof(TimeValue) == 16 && offsetof(TimeValue, type) == 0 && offsetof(TimeValue, value) == 8);
static_assert(to_underlying(TimeValue::Type::Milliseconds) == 0 && to_underlying(TimeValue::Type::Percentage) == 1);
static_assert(to_underlying(FillMode::None) == to_underlying(RustFFI::FillMode::None));
static_assert(to_underlying(FillMode::Forwards) == to_underlying(RustFFI::FillMode::Forwards));
static_assert(to_underlying(FillMode::Backwards) == to_underlying(RustFFI::FillMode::Backwards));
static_assert(to_underlying(FillMode::Both) == to_underlying(RustFFI::FillMode::Both));
static_assert(to_underlying(FillMode::Auto) == to_underlying(RustFFI::FillMode::Auto));
static_assert(to_underlying(PlaybackDirection::Normal) == to_underlying(RustFFI::PlaybackDirection::Normal));
static_assert(to_underlying(PlaybackDirection::Reverse) == to_underlying(RustFFI::PlaybackDirection::Reverse));
static_assert(to_underlying(PlaybackDirection::Alternate) == to_underlying(RustFFI::PlaybackDirection::Alternate));
static_assert(to_underlying(PlaybackDirection::AlternateReverse) == to_underlying(RustFFI::PlaybackDirection::AlternateReverse));

// https://drafts.csswg.org/web-animations-2/#normalize-specified-timing
void AnimationEffect::normalize_specified_timing()
{
    RustFFI::SpecifiedEffectTiming specified {
        .start_delay = m_specified_start_delay,
        .end_delay = m_specified_end_delay,
        .iteration_duration = m_specified_iteration_duration.visit(
            [](double duration) { return RustFFI::SpecifiedIterationDuration { .is_auto = false, .milliseconds = duration }; },
            [](Utf16String const&) { return RustFFI::SpecifiedIterationDuration { .is_auto = true, .milliseconds = 0 }; }),
        .iteration_start = m_iteration_start,
        .iteration_count = m_iteration_count,
        .fill_mode = static_cast<RustFFI::FillMode>(to_underlying(m_fill_mode)),
        .playback_direction = static_cast<RustFFI::PlaybackDirection>(to_underlying(m_playback_direction)),
    };
    auto time_unit = TimeValue::create_zero(associated_timeline()).type;
    auto timing = RustFFI::animations_normalize_specified_timing(&specified, to_ffi(timeline_duration()), time_unit);
    m_start_delay = timing.start_delay;
    m_end_delay = timing.end_delay;
    m_iteration_duration = timing.iteration_duration;
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-updatetiming
// https://www.w3.org/TR/web-animations-1/#update-the-timing-properties-of-an-animation-effect
// https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
WebIDL::ExceptionOr<void> AnimationEffect::update_timing(Bindings::OptionalEffectTiming const& timing)
{
    // 1. If the iterationStart member of input exists and is less than zero, throw a TypeError and abort this
    //    procedure.
    if (timing.iteration_start.has_value() && timing.iteration_start.value() < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid iteration start value"_utf16 };

    // 2. If the iterations member of input exists, and is less than zero or is the value NaN, throw a TypeError and
    //    abort this procedure.
    if (timing.iterations.has_value() && (timing.iterations.value() < 0.0 || isnan(timing.iterations.value())))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid iteration count value"_utf16 };

    // 3. If the duration member of input exists, and is less than zero or is the value NaN, throw a TypeError and
    //    abort this procedure.
    // Note: "auto", the only valid string value, is treated as 0.
    auto& duration = timing.duration;
    auto has_valid_duration_value = [&] {
        if (!duration.has_value())
            return true;
        if (duration->has<double>() && (duration->get<double>() < 0.0 || isnan(duration->get<double>())))
            return false;
        if (duration->has<Utf16String>() && (duration->get<Utf16String>() != "auto"sv))
            return false;
        return true;
    }();
    if (!has_valid_duration_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid duration value"_utf16 };

    // 4. If the easing member of input exists but cannot be parsed using the <easing-function> production
    //    [CSS-EASING-1], throw a TypeError and abort this procedure.
    Optional<CSS::EasingFunction> easing_value;
    if (timing.easing.has_value()) {
        easing_value = parse_easing_string(timing.easing.value());
        if (!easing_value.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid easing function"_utf16 };
    }

    // 5. Assign each member that exists in input to the corresponding timing property of effect as follows:

    //    - delay → specified start delay
    if (timing.delay.has_value())
        m_specified_start_delay = timing.delay.value();

    //    - endDelay → specified end delay
    if (timing.end_delay.has_value())
        m_specified_end_delay = timing.end_delay.value();

    //    - fill → fill mode
    if (timing.fill.has_value())
        m_fill_mode = timing.fill.value();

    //    - iterationStart → iteration start
    if (timing.iteration_start.has_value())
        m_iteration_start = timing.iteration_start.value();

    //    - iterations → iteration count
    if (timing.iterations.has_value())
        m_iteration_count = timing.iterations.value();

    //    - duration → specified iteration duration
    if (timing.duration.has_value())
        m_specified_iteration_duration = timing.duration.value();

    //    - direction → playback direction
    if (timing.direction.has_value())
        m_playback_direction = timing.direction.value();

    //    - easing → timing function
    if (easing_value.has_value())
        m_timing_function = easing_value.value();

    // 6. Follow the procedure to normalize specified timing.
    normalize_specified_timing();

    if (auto* keyframe_effect = as_if<KeyframeEffect>(*this))
        keyframe_effect->clear_per_frame_style_update_cache();

    // AD-HOC: Notify the associated animation that the effect timing has changed.
    if (auto animation = m_associated_animation)
        animation->effect_timing_changed({});

    return {};
}

void AnimationEffect::set_associated_animation(GC::Ptr<Animation> value)
{
    m_associated_animation = value;
    if (auto* keyframe_effect = as_if<KeyframeEffect>(*this))
        keyframe_effect->invalidate_animation_preparation();

    // NB: The normalization of the specified timing depends on the timeline of the associated animation.
    normalize_specified_timing();
}

GC::Ptr<Bindings::Wrappable> AnimationEffect::relevant_global_impl() const
{
    if (m_associated_animation)
        return static_cast<Bindings::Wrappable&>(*m_associated_animation).relevant_global_impl();
    return nullptr;
}

void AnimationEffect::update_style_if_needed() const
{
    if (m_associated_animation)
        m_associated_animation->update_style_if_needed();
}

// https://www.w3.org/TR/web-animations-1/#local-time
Optional<TimeValue> AnimationEffect::local_time() const
{
    if (m_has_local_time_override_for_observation)
        return m_local_time_override_for_observation;

    // The local time of an animation effect at a given moment is based on the first matching condition from the
    // following:

    // -> If the animation effect is associated with an animation,
    if (m_associated_animation) {
        // the local time is the current time of the animation.
        return m_associated_animation->current_time();
    }

    // -> Otherwise,
    //    the local time is unresolved.
    return {};
}

RustFFI::AnimationEffectTiming AnimationEffect::effect_timing(Bindings::FillMode fill_mode) const
{
    return {
        .start_delay = m_start_delay,
        .end_delay = m_end_delay,
        .iteration_duration = m_iteration_duration,
        .iteration_start = m_iteration_start,
        .iteration_count = m_iteration_count,
        .fill_mode = static_cast<RustFFI::FillMode>(to_underlying(fill_mode)),
        .playback_direction = static_cast<RustFFI::PlaybackDirection>(to_underlying(m_playback_direction)),
    };
}

RustFFI::EffectTimingSnapshot AnimationEffect::timing_snapshot() const
{
    VERIFY(m_associated_animation);
    auto timeline = m_associated_animation->timeline();
    return {
        .effect = effect_timing(m_fill_mode),
        .playback = m_associated_animation->playback(),
        .timeline = timeline ? timeline->sample() : RustFFI::TimelineSample {},
        .has_timeline = timeline != nullptr,
        .clock = timeline ? timeline->clock() : RustFFI::TimelineClock { .source = RustFFI::TimelineClockSource::Unticked, .origin_time = 0, .scroll_timeline = 0 },
    };
}

AnimationEffect::ResolvedTiming AnimationEffect::resolve_timing_at(Optional<TimeValue> const& local_time, Bindings::FillMode fill_mode, WithTransformedProgress with_transformed_progress, EndpointInclusiveActiveInterval endpoint_inclusive) const
{
    auto timing = effect_timing(fill_mode);
    Optional<RustFFI::AnimationPlayback> playback;
    Optional<RustFFI::TimelineSample> timeline_sample;
    if (m_associated_animation) {
        playback = m_associated_animation->playback();
        if (auto timeline = m_associated_animation->timeline())
            timeline_sample = timeline->sample();
    }
    Optional<CSS::EasingFunction::Descriptor> timing_function;
    if (with_transformed_progress == WithTransformedProgress::Yes)
        timing_function = m_timing_function.descriptor();
    return RustFFI::animations_resolve_effect_timing(
        &timing,
        to_ffi(local_time),
        playback.has_value() ? &*playback : nullptr,
        timeline_sample.has_value() ? &*timeline_sample : nullptr,
        timing_function.has_value() ? &*timing_function : nullptr,
        endpoint_inclusive == EndpointInclusiveActiveInterval::Yes);
}

// What follows from the effect's timing alone, without a local time.
AnimationEffect::ResolvedTiming AnimationEffect::resolve_timing_alone() const
{
    auto timing = effect_timing(m_fill_mode);
    return RustFFI::animations_resolve_effect_timing(&timing, to_ffi({}), nullptr, nullptr, nullptr, false);
}

AnimationEffect::ResolvedTiming AnimationEffect::resolve_timing() const
{
    return resolve_timing_at(local_time(), m_fill_mode);
}

// https://www.w3.org/TR/web-animations-1/#end-time
TimeValue AnimationEffect::end_time() const
{
    return resolve_timing_alone().end_time;
}

// https://www.w3.org/TR/web-animations-1/#active-duration
TimeValue AnimationEffect::active_duration() const
{
    return resolve_timing_alone().active_duration;
}

// https://www.w3.org/TR/web-animations-1/#before-active-boundary-time
TimeValue AnimationEffect::before_active_boundary_time() const
{
    return resolve_timing_alone().before_active_boundary_time;
}

// https://www.w3.org/TR/web-animations-1/#active-after-boundary-time
TimeValue AnimationEffect::after_active_boundary_time() const
{
    return resolve_timing_alone().after_active_boundary_time;
}

// https://www.w3.org/TR/web-animations-1/#calculating-the-active-time
Optional<TimeValue> AnimationEffect::active_time() const
{
    return from_ffi(resolve_timing().active_time);
}

Optional<TimeValue> AnimationEffect::active_time_using_fill(Bindings::FillMode fill_mode) const
{
    return from_ffi(resolve_timing_at(local_time(), fill_mode).active_time);
}

// https://www.w3.org/TR/web-animations-1/#current
bool AnimationEffect::is_current() const
{
    return resolve_timing().is_current;
}

// https://www.w3.org/TR/web-animations-1/#in-effect
bool AnimationEffect::is_in_effect() const
{
    return resolve_timing().is_in_effect;
}

AnimationEffect::Phase AnimationEffect::phase() const
{
    return resolve_timing().phase;
}

Optional<double> AnimationEffect::current_iteration() const
{
    return current_iteration(resolve_timing());
}

// https://www.w3.org/TR/web-animations-1/#current-iteration
Optional<double> AnimationEffect::current_iteration(ResolvedTiming const& timing) const
{
    return from_ffi(timing.current_iteration);
}

// https://www.w3.org/TR/web-animations-1/#transformed-progress
Optional<double> AnimationEffect::transformed_progress(EndpointInclusiveActiveInterval endpoint_inclusive) const
{
    return from_ffi(resolve_timing_at(local_time(), m_fill_mode, WithTransformedProgress::Yes, endpoint_inclusive).transformed_progress);
}

Optional<CSS::EasingFunction> AnimationEffect::parse_easing_string(Utf16View value)
{
    if (auto style_value = parse_css_value(CSS::Parser::ParsingParams(), value, CSS::PropertyID::AnimationTimingFunction)) {
        if (style_value->is_unresolved() || style_value->is_css_wide_keyword())
            return {};

        auto easing_values = style_value->as_value_list().values();

        if (easing_values.size() != 1)
            return {};

        // FIXME: We should absolutize the style value to resolve relative lengths within calcs
        return CSS::EasingFunction::from_style_value(easing_values[0]);
    }

    return {};
}

AnimationEffect::AnimationEffect() = default;

void AnimationEffect::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_animation);
}

AnimationUpdateContext::~AnimationUpdateContext()
{
    publish();
}

void AnimationUpdateContext::publish()
{
    // Each element's effects, sampled over the style it holds.
    struct Sample {
        DOM::AbstractElement element;
        ElementData const& data;
        Vector<GC::Ref<KeyframeEffect>> effects;

        void visit_edges(GC::Cell::Visitor& visitor)
        {
            element.visit_edges(visitor);
            visitor.visit(effects);
        }
    };
    GC::RootVector<Sample> samples;
    for (auto& it : elements) {
        if (!it.value.target_style)
            continue;
        auto& element = it.key;
        GC::Ref<DOM::Element> target = element.element();
        // Disconnected elements no longer have a style-engine row to publish refreshed
        // animation style into.
        if (target->style_node_id() == 0)
            continue;
        // Provisionally started transitions are not associated with the element yet, so they are
        // never among the collected effects, but their values are already part of the published
        // style. Collect them first, in composite order below every associated effect, or this
        // update would rebuild the style without them.
        GC::RootVector<GC::Ref<KeyframeEffect>> effects_to_collect;
        target->document().style_computer().for_each_provisional_transition_effect(element, [&](KeyframeEffect& effect) {
            effects_to_collect.append(effect);
        });
        for (auto& animation : target->associated_animations_in_composite_order()) {
            if (animation->is_idle() || !animation->effect() || !is<KeyframeEffect>(*animation->effect()))
                continue;
            auto& effect = static_cast<KeyframeEffect&>(*animation->effect());
            if (effect.target() != target || effect.pseudo_element_type() != element.pseudo_element())
                continue;
            effects_to_collect.append(effect);
        }
        // A dirty effect may have just become irrelevant. Include it once so rebuilding the
        // animated overlay removes its terminal contribution.
        for (auto& dirty_effect : it.value.effects) {
            if (!effects_to_collect.contains_slow(dirty_effect))
                effects_to_collect.append(dirty_effect);
        }
        samples.append({ element, it.value, move(effects_to_collect) });
    }

    // An element whose overlay the update publishes, and whether the record it installed may become its transition
    // baseline.
    struct Publication {
        Sample& sample;
        bool may_record_baseline { false };
    };
    Vector<Publication> publications;
    GC::RootVector<CSS::StyleComputer::SampledAnimationOverlay> overlays;
    Vector<CSS::StyleEngineFFI::FfiAnimationOverlayPublication> published;

    // The render owner samples every element of a document in one call, and publishes what it sampled in another,
    // before any of their styles is installed.
    for (size_t first = 0; first < samples.size();) {
        auto& document = samples[first].element.document();
        auto& style_computer = document.style_computer();
        Layout::ForcedReadScope read { document };
        GC::RootVector<CSS::StyleComputer::AnimationRefreshRequest> requests;
        size_t end = first;
        for (; end < samples.size() && &samples[end].element.document() == &document; ++end) {
            auto& sample = samples[end];
            if (sample.element.installed_style() != sample.data.style_before_update)
                continue;
            // NB: An element left with no effects, as where its style just removed its last animation, has nothing to
            //     sample, and what it animated of its custom properties ends here.
            if (sample.effects.is_empty())
                style_computer.stop_publishing_animated_custom_properties(sample.element);
            else
                requests.append({ sample.element, sample.effects.span(), *sample.data.target_style });
        }
        // A timeline script observed ahead of its current time in this task is sampled at that time, as an update of the
        // effects needing it is, or the style installed here and the one that update sampled would keep disagreeing about
        // an effect that ended between the two times.
        AnimationTimeline::at_times_observed_ahead(document.associated_animation_timelines().begin(), document.associated_animation_timelines().end(), [&] {
            style_computer.refresh_animations_into_each(read, requests);
        });

        publications.clear_with_capacity();
        overlays.clear_with_capacity();
        for (auto& sample : samples.span().slice(first, end - first)) {
            auto const& style = *sample.data.target_style;
            if (sample.element.installed_style() != sample.data.style_before_update
                || !sample.element.installed_style().animation_overlay_changed(style.animated_overlay()))
                continue;
            bool const may_record_baseline = style.animated_overlay() && !CSS::ComputedValuesFFI::rust_animated_overlay_is_empty(style.animated_overlay())
                && document.is_in_style_stabilization_epoch();
            publications.append({ sample, may_record_baseline });
            overlays.append({ sample.element, style });
        }
        published.resize(overlays.size());
        style_computer.publish_sampled_animation_overlays(read, overlays, published);
        // NB: Each publication hands over a reference to the style it published, which a later one can retire.
        Vector<CSS::InstalledStyle> published_styles;
        published_styles.ensure_capacity(published.size());
        for (auto const& publication : published)
            published_styles.unchecked_append(CSS::InstalledStyle::adopt(publication.record));
        first = end;

        for (size_t i = 0; i < publications.size(); ++i) {
            auto const& [sample, may_record_baseline] = publications[i];
            auto const& animated_property_invalidation = published[i].invalidation;
            auto& element = sample.element;
            auto const& data = sample.data;
            GC::Ref<DOM::Element> target = element.element();
            // An earlier entry already republished this style with the current animation values.
            bool const republished = element.installed_style() != data.style_before_update;
            if (!republished && may_record_baseline && (document.style_stabilization_has_style_reactions() || animated_property_invalidation.requires_base_style_recomputation))
                style_computer.record_transition_stabilization_baseline(element, data.style_before_update);
            if (republished)
                continue;
            auto invalidation = CSS::decode_style_invalidation(animated_property_invalidation.invalidation);
            target->refresh_computed_style(read, element.pseudo_element(), move(published_styles[i]));
            if (auto* svg_element = as_if<SVG::SVGElement>(*target); svg_element && !element.pseudo_element().has_value())
                svg_element->note_svg_paint_resource_description_may_have_changed();

            // Box-type, overflow, and text-alignment adjustments consume the unadjusted base values,
            // which an animation-only overlay update deliberately does not reconstruct. Publish an
            // exact feedback action so the ordinary reaction path re-cascades that base before the
            // frame becomes observable.
            if (animated_property_invalidation.requires_base_style_recomputation && !data.base_is_current)
                target->document().style_computer().style_engine().record_derived_element_style_input_change(target->style_node_id(), CSS::StyleEngine::PublishedStyle | CSS::StyleEngine::RecomputeStyle);

            // NB: Publishing the composition had the style engine restyle the element's pseudo-elements and the
            //     children that compute from what the composition moved, in its next pass.

            // NB: refresh_computed_style() already publishes the new record to the layout node. Only
            // inherited values and image resources require the additional C++ style side effects.
            auto apply_layout_node_style_side_effects = [&](Layout::NodeWithStyle& layout_node, CSS::StyleRecordID style_record) {
                if (animated_property_invalidation.requires_layout_node_style_application)
                    layout_node.apply_style(style_record);
                else if (animated_property_invalidation.requires_style_resource_update)
                    layout_node.attach_style_resources();
            };
            if (!element.pseudo_element().has_value()) {
                if (auto* layout_node = target->unsafe_layout_node(read))
                    apply_layout_node_style_side_effects(*layout_node, target->style_record_identity());
            } else if (auto pseudo_element_node = target->pseudo_element_unsafe_layout_node(read, element.pseudo_element().value())) {
                apply_layout_node_style_side_effects(*pseudo_element_node, target->style_record_identity(element.pseudo_element()));
            }

            if (invalidation.needs_relayout())
                target->set_needs_layout_update(DOM::SetNeedsLayoutReason::KeyframeEffect);
            if (invalidation.needs_layout_tree_rebuild()) {
                auto rebuild_root = element.pseudo_element().has_value()
                    ? CSS::LayoutTreeRebuildRoot::Parent
                    : invalidation.layout_tree_rebuild_root();
                target->set_needs_layout_tree_rebuild(read, DOM::SetNeedsLayoutTreeUpdateReason::KeyframeEffect, rebuild_root);
            }
            if (invalidation.accumulated_visual_contexts() != CSS::AccumulatedVisualContextInvalidation::None) {
                auto scope = invalidation.accumulated_visual_contexts() == CSS::AccumulatedVisualContextInvalidation::Rebuild
                    ? DOM::Document::AccumulatedVisualContextUpdateScope::Structure
                    : DOM::Document::AccumulatedVisualContextUpdateScope::Values;
                // NB: Element-reference pseudo elements (e.g. ::placeholder) are not synthetic, so schedule their
                //     layout node directly instead of going through the owning element.
                if (element.pseudo_element().has_value()) {
                    if (auto pseudo_element_node = target->pseudo_element_unsafe_layout_node(read, element.pseudo_element().value()))
                        element.document().schedule_accumulated_visual_context_update(*pseudo_element_node, scope);
                } else {
                    element.document().schedule_accumulated_visual_context_update(read, target, scope);
                }
            }

            auto* repaint_layout_node = element.pseudo_element().has_value()
                ? target->pseudo_element_unsafe_layout_node(read, *element.pseudo_element())
                : target->unsafe_layout_node(read);
            if (repaint_layout_node && Painting::has_committed_box(*repaint_layout_node))
                Painting::repaint_after_style_change(*repaint_layout_node, invalidation);
        }
    }
    elements.clear();
}

}
