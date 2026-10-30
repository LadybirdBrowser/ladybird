/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimationElement.h>

#include <AK/Math.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SMILAnimation.h>
#include <LibWeb/SVG/SMILTimeContainer.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/TimeEvent.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::SVG {

SVGAnimationElement::SVGAnimationElement(DOM::Document& document, DOM::QualifiedName name)
    : SVGElement(document, name)
{
}

void SVGAnimationElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

void SVGAnimationElement::inserted()
{
    Base::inserted();
    update_animation();
}

void SVGAnimationElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_animation();
}

void SVGAnimationElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    update_animation();
}

void SVGAnimationElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (first_is_one_of(name, AttributeNames::attributeName, AttributeNames::begin, AttributeNames::dur, AttributeNames::fill, AttributeNames::repeatCount) && !namespace_.has_value())
        update_animation();
}

GC::Ptr<SMILTimeContainer> SVGAnimationElement::time_container()
{
    auto outermost_svg_element = this->outermost_svg_element();
    return outermost_svg_element ? outermost_svg_element->time_container() : nullptr;
}

// https://svgwg.org/specs/animations/#HrefAttribute
GC::Ptr<SVGElement> SVGAnimationElement::target_element()
{
    // FIXME: A URL reference to the element which is the target of this animation element and which therefore will be
    //        modified over time.
    // If the ‘href’ attribute or the deprecated ‘xlink:href’ attribute is not provided, then the target element will
    // be the immediate parent element of the current animation element.
    return as_if<SVGElement>(parent_element().ptr());
}

// https://svgwg.org/specs/animations/#AttributeNameAttribute
Optional<CSS::PropertyID> SVGAnimationElement::target_property() const
{
    auto attribute_name = get_attribute(AttributeNames::attributeName);
    if (!attribute_name.has_value())
        return {};

    // That is, when determining if ‘attributeName’ corresponds to an attribute name or a CSS property name, the
    // implementation must first search through the list of CSS properties for a matching property name.
    // NB: Neither custom properties nor properties that cannot be animated have an animation.
    auto property_id = CSS::property_id_from_string(*attribute_name);
    if (!property_id.has_value() || property_id == CSS::PropertyID::Custom || !CSS::is_animatable_property(*property_id))
        return {};

    // FIXME: If no matching property is found, the implementation must search for a matching attribute on the target
    //        element.
    return property_id;
}

// https://svgwg.org/specs/animations/#DurAttribute
double SVGAnimationElement::simple_duration() const
{
    // If the animation does not have a ‘dur’ attribute, the simple duration is indefinite.
    auto dur = get_attribute(AttributeNames::dur);
    if (!dur.has_value())
        return AK::Infinity<double>;

    // Clock-value
    //     Specifies the length of the simple duration in presentation time. Value must be greater than 0.
    if (auto duration = parse_clock_value_in_milliseconds(*dur); duration.has_value() && *duration > 0)
        return *duration;

    // NB: An "indefinite" value specifies an indefinite simple duration, SVG ignores a "media" value, and SMIL
    //     Animation ignores a value with any error in its syntax. Each leaves the simple duration indefinite.
    return AK::Infinity<double>;
}

// https://svgwg.org/specs/animations/#RepeatCountAttribute
double SVGAnimationElement::repeat_count() const
{
    // Specifies the number of iterations of the animation function. It can have the following attribute values:
    auto repeat_count = get_attribute(AttributeNames::repeatCount);
    if (!repeat_count.has_value())
        return 1;

    // <number>
    //     This is a (base 10) "floating point" numeric value that specifies the number of iterations. It can include
    //     partial iterations expressed as fraction values. A fractional value describes a portion of the simple
    //     duration. Values must be greater than 0.
    if (auto count = parse_number_percentage(*repeat_count); count.has_value() && !count->is_percentage()
        && count->value() > 0 && isfinite(count->value())) {
        return count->value();
    }

    // indefinite
    //     The animation is defined to repeat indefinitely (i.e. until the document ends).
    if (*repeat_count == "indefinite"sv)
        return AK::Infinity<double>;

    // NB: SMIL Animation ignores a value with any error in its syntax, which leaves a single iteration.
    return 1;
}

// https://svgwg.org/specs/animations/#FillAttribute
Bindings::FillMode SVGAnimationElement::fill_mode() const
{
    // freeze
    //     The animation effect F(t) is defined to freeze the effect value at the last value of the active duration.
    if (auto fill = get_attribute(AttributeNames::fill); fill.has_value() && *fill == "freeze"sv)
        return Bindings::FillMode::Forwards;

    // remove
    //     The animation effect is removed (no longer applied) when the active duration of the animation is over.
    return Bindings::FillMode::None;
}

// https://svgwg.org/specs/animations/#RestartAttribute
SVGAnimationElement::Restart SVGAnimationElement::restart() const
{
    // NB: SMIL Animation ignores a value with any error in its syntax, which leaves the default value of always.
    auto restart = get_attribute(AttributeNames::restart);
    if (restart == "whenNotActive"sv)
        return Restart::WhenNotActive;
    if (restart == "never"sv)
        return Restart::Never;
    return Restart::Always;
}

// https://svgwg.org/specs/animations/#BeginValueListSyntax
static Optional<double> parse_offset_value_in_milliseconds(Utf16View value)
{
    // offset-value ::= ( S? "+" | "-" S? )? ( Clock-value )
    auto sign = 1.;
    if (value.starts_with(u"+"sv) || value.starts_with(u"-"sv)) {
        if (value.starts_with(u"-"sv))
            sign = -1.;
        value = value.substring_view(1).trim_ascii_whitespace(TrimMode::Left);
    }
    auto clock_value = parse_clock_value_in_milliseconds(value);
    if (!clock_value.has_value())
        return {};
    return sign * *clock_value;
}

// https://svgwg.org/specs/animations/#BeginAttribute
Optional<double> SVGAnimationElement::begin_time_from_begin_attribute() const
{
    // NB: The initial value is 0s.
    auto begin = get_attribute(AttributeNames::begin);
    if (!begin.has_value())
        return 0;

    // The attribute value is a semicolon separated list of values.
    // The interpretation of a list of begin times is detailed in SMIL Animation's section on "Evaluation of begin and
    // end time lists".
    // offset-value
    //     For SVG, the implicit syncbase begin is defined to be relative to the document begin.
    // "indefinite"
    //     The begin of the animation will be determined by a "beginElement()" method call or a hyperlink targeted to
    //     the element.
    // NB: The current interval begins at the earliest of the offset values, and without any, it has no begin time.
    // FIXME: Begin a new interval at each of the later offset values.
    // FIXME: Support the syncbase, event, repeat, accessKey and wallclock-sync values.
    Optional<double> earliest_begin_time;
    begin->utf16_view().for_each_split_view(';', SplitBehavior::KeepEmpty, [&](Utf16View item) {
        auto begin_time = parse_offset_value_in_milliseconds(item.trim_ascii_whitespace());
        if (begin_time.has_value() && (!earliest_begin_time.has_value() || *begin_time < *earliest_begin_time))
            earliest_begin_time = begin_time;
        return IterationDecision::Continue;
    });
    return earliest_begin_time;
}

// https://www.w3.org/TR/smil-animation/#Timing-EvaluationOfBeginEndTimeLists
Optional<double> SVGAnimationElement::interval_begin_time() const
{
    // NB: An instance time that a DOM method created replaces the begin times of the begin attribute, as it was only
    //     kept if it begins the current interval.
    if (m_begin_instance_time_from_dom.has_value())
        return m_begin_instance_time_from_dom;
    return begin_time_from_begin_attribute();
}

// https://www.w3.org/TR/smil-animation/#Timing-EvaluationOfBeginEndTimeLists
Optional<double> SVGAnimationElement::interval_end_time() const
{
    // NB: An end time before the begin time of the interval does not end it.
    // FIXME: Support the end attribute.
    auto begin_time = interval_begin_time();
    if (!begin_time.has_value() || !m_end_instance_time_from_dom.has_value() || *m_end_instance_time_from_dom < *begin_time)
        return {};
    return m_end_instance_time_from_dom;
}

bool SVGAnimationElement::is_active_at(double time) const
{
    auto begin_time = interval_begin_time();
    if (!begin_time.has_value() || time < *begin_time)
        return false;
    auto active_end = *begin_time + simple_duration() * repeat_count();
    if (auto end_time = interval_end_time(); end_time.has_value())
        active_end = min(active_end, *end_time);
    return time < active_end;
}

double SVGAnimationElement::current_presentation_time()
{
    // NB: Like SVGSVGElement.getCurrentTime(), the time is 0 until the timeline has begun.
    auto time_container = this->time_container();
    if (!time_container || !time_container->has_begun())
        return 0;
    return time_container->current_time_in_milliseconds();
}

Animations::BaseKeyframe SVGAnimationElement::keyframe(double offset, CSS::PropertyID property_id, CSS::StyleValue const& value)
{
    Animations::BaseKeyframe keyframe;
    keyframe.offset = offset;
    keyframe.properties = Animations::BaseKeyframe::ParsedProperties {};
    set_keyframe_property(keyframe, property_id, value);
    return keyframe;
}

void SVGAnimationElement::set_keyframe_property(Animations::BaseKeyframe& keyframe, CSS::PropertyID property_id, CSS::StyleValue const& value)
{
    keyframe.parsed_properties().set(CSS::PropertyNameAndID::from_id(property_id), CSS::RustStyleValueHandle::retained(value.rust_style_value_data()));
}

void SVGAnimationElement::update_animation()
{
    if (m_animation) {
        m_animation->cancel();
        m_animation = nullptr;
    }

    if (!is_connected())
        return;
    auto time_container = this->time_container();
    if (!time_container)
        return;
    auto target = target_element();
    if (!target)
        return;
    auto property_id = target_property();
    if (!property_id.has_value())
        return;
    auto keyframes = this->keyframes(*target, *property_id);
    if (keyframes.is_empty())
        return;
    auto begin_time = interval_begin_time();
    if (!begin_time.has_value())
        return;

    // NB: The animation's current time is kept equal to the time of the timeline of the SVG document fragment, so the
    //     interval begins after a delay of its begin time.
    // FIXME: Resolve the interval from the other timing attributes.
    // FIXME: A running animation requests a rendering update on every frame, even when its output cannot change, as
    //        with an indefinite one. This keeps the rendering loop running for as long as such an animation is active.
    Animations::KeyframeEffect::Options options;
    auto simple_duration = this->simple_duration();
    auto repeat_count = this->repeat_count();
    options.timing.delay = *begin_time;
    options.timing.duration = simple_duration;
    options.timing.iterations = repeat_count;
    options.timing.fill = fill_mode();

    // NB: An end time cuts the active duration short. With an indefinite simple duration, the animation holds the
    //     value at its start for the active duration that remains.
    if (auto end_time = interval_end_time(); end_time.has_value()) {
        auto active_duration = *end_time - *begin_time;
        if (isinf(simple_duration)) {
            options.timing.duration = active_duration;
            options.timing.iterations = 1;
            options.timing.easing = "linear(0, 0)"_utf16;
        } else {
            options.timing.iterations = min(repeat_count, active_duration / simple_duration);
        }
    }
    auto effect = MUST(Animations::KeyframeEffect::create_from_processed_keyframes(target, move(keyframes), options));

    auto animation = SMILAnimation::create(*this, *time_container, *begin_time);
    animation->set_timeline(document().timeline());
    animation->set_effect(effect);
    time_container->add_animation(animation);
    m_animation = animation;

    // NB: Time events are raised for intervals that begin from now on.
    if (!m_time_events_last_checked_time.has_value())
        m_time_events_last_checked_time = current_presentation_time();
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
void SVGAnimationElement::set_onbegin(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::beginEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onbegin()
{
    return event_handler_attribute(HTML::EventNames::beginEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
void SVGAnimationElement::set_onend(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::endEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onend()
{
    return event_handler_attribute(HTML::EventNames::endEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
void SVGAnimationElement::set_onrepeat(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::repeatEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onrepeat()
{
    return event_handler_attribute(HTML::EventNames::repeatEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getStartTime
WebIDL::ExceptionOr<float> SVGAnimationElement::get_start_time()
{
    // Returns the begin time, in seconds, for this animation element's current interval, if it exists, regardless of
    // whether the interval has begun yet. If there is no current interval, then a DOMException with code
    // INVALID_STATE_ERR is thrown.
    auto begin_time = is_connected() && time_container() ? interval_begin_time() : Optional<double> {};
    if (!begin_time.has_value())
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(*this), "The animation element has no current interval"_utf16);
    return static_cast<float>(*begin_time / 1000.);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getCurrentTime
float SVGAnimationElement::get_current_time()
{
    // Returns the current time in seconds relative to time zero for the given time container.
    // NB: Like SVGSVGElement.getCurrentTime(), return 0 until the timeline has begun, and for an animation element
    //     outside of an SVG document fragment.
    auto time_container = is_connected() ? this->time_container() : nullptr;
    if (!time_container || !time_container->has_begun())
        return 0;
    return static_cast<float>(time_container->current_time_in_milliseconds() / 1000.);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getSimpleDuration
WebIDL::ExceptionOr<float> SVGAnimationElement::get_simple_duration()
{
    // Returns the number of seconds for the simple duration for this animation. If the simple duration is undefined
    // (e.g., the end time is indefinite), then an exception is raised.
    // NB: The exception is a DOMException with code NOT_SUPPORTED_ERR.
    auto duration = simple_duration();
    if (isinf(duration))
        return WebIDL::NotSupportedError::create(HTML::relevant_realm(*this), "The simple duration is indefinite"_utf16);
    return static_cast<float>(duration / 1000.);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__beginElement
void SVGAnimationElement::begin_element()
{
    // Creates a begin instance time for the current time. The new instance time is added to the begin instance times
    // list. The behavior of this method is equivalent to beginElementAt(0).
    begin_element_at(0);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__beginElementAt
void SVGAnimationElement::begin_element_at(float offset)
{
    // NB: An animation element outside of an SVG document fragment has no timeline to create an instance time on.
    if (!is_connected() || !time_container())
        return;

    // Creates a begin instance time for the current time plus the specified offset. The new instance time is added to
    // the begin instance times list.
    // The instance time associated with a beginElementAt() or endElementAt() call is the current presentation time at
    // the time of the DOM method call, plus or minus the specified offset.
    auto current_time = current_presentation_time();
    auto instance_time = current_time + static_cast<double>(offset) * 1000.;

    // Note that beginElement() is subject to the ‘restart’ attribute in the same manner that event-based begin timing
    // is.
    switch (restart()) {
    case Restart::Always:
        // The animation can be restarted at any time.
        break;
    case Restart::WhenNotActive:
        // The animation can only be restarted when it is not active (i.e. after the active end). Attempts to restart
        // the animation during its active duration are ignored.
        if (is_active_at(current_time))
            return;
        break;
    case Restart::Never:
        // The element cannot be restarted for the remainder of the current simple duration of the parent time
        // container.
        if (auto begin_time = interval_begin_time(); begin_time.has_value() && *begin_time <= current_time)
            return;
        break;
    }

    // NB: An instance time that is still to come does not replace the begin of a current interval that begins
    //     before it, and that has not ended.
    // FIXME: Begin a new interval at such an instance time once it is reached.
    if (instance_time > current_time) {
        if (auto begin_time = interval_begin_time(); begin_time.has_value() && *begin_time <= instance_time
            && (current_time < *begin_time || is_active_at(current_time))) {
            return;
        }
    }

    // NB: The new interval ends only at an end instance time after its begin, so that one created at the same time
    //     by an earlier endElement() call does not end it at once.
    m_begin_instance_time_from_dom = instance_time;
    if (m_end_instance_time_from_dom.has_value() && *m_end_instance_time_from_dom <= instance_time)
        m_end_instance_time_from_dom.clear();
    update_animation();
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__endElement
void SVGAnimationElement::end_element()
{
    // Creates an end instance time for the current time. The new instance time is added to the end instance times
    // list. The behavior of this method is equivalent to endElementAt(0).
    end_element_at(0);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__endElementAt
void SVGAnimationElement::end_element_at(float offset)
{
    // NB: An animation element outside of an SVG document fragment has no timeline to create an instance time on.
    if (!is_connected() || !time_container())
        return;

    // Creates a end instance time for the current time plus the specified offset. The new instance time is added to
    // the end instance times list.
    // The instance time associated with a beginElementAt() or endElementAt() call is the current presentation time at
    // the time of the DOM method call, plus or minus the specified offset.
    m_end_instance_time_from_dom = current_presentation_time() + static_cast<double>(offset) * 1000.;
    update_animation();
}

void SVGAnimationElement::queue_time_event(Utf16FlyString const& type, i32 detail)
{
    // NB: Time events are dispatched along with the other animation events of the document.
    auto& document = this->document();
    auto event = TimeEvent::create(type, document.window() ? document.window()->window().ptr() : nullptr, detail,
        HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this)));
    event->set_is_trusted(true);

    auto timeline = m_animation->timeline();
    document.append_pending_animation_event({
        .event = event,
        .animation = *m_animation,
        .target = *this,
        .scheduled_event_time = timeline && timeline->can_convert_a_timeline_time_to_an_origin_relative_time()
            ? timeline->convert_a_timeline_time_to_an_origin_relative_time(timeline->current_time())
            : Optional<double> {},
    });
}

// https://svgwg.org/specs/animations/#InterfaceTimeEvent
void SVGAnimationElement::queue_time_events_if_necessary()
{
    // FIXME: Raise time events for an animation element without a target or animated property too.
    auto time_container = is_connected() ? this->time_container() : nullptr;
    if (!m_animation || !time_container || !time_container->has_begun())
        return;

    auto current_time = time_container->current_time_in_milliseconds();
    auto last_checked_time = exchange(m_time_events_last_checked_time, current_time);
    auto begin_time = interval_begin_time();
    auto is_active = is_active_at(current_time);

    // endEvent
    //     This event is raised at the active end of the element. Note that this event is not raised at the simple end
    //     of each repeat.
    //     Note that if an element is restarted while it is currently playing, the element will raise an end event and
    //     another begin event, as the element restarts.
    if (m_time_events_reported_active && (!is_active || begin_time != m_time_events_begin_time)) {
        m_time_events_reported_active = false;
        queue_time_event(HTML::EventNames::endEvent);
    }

    // beginEvent
    //     This event is raised when the element local timeline begins to play. It will be raised each time the element
    //     begins the active duration (i.e. when it restarts, but not when it repeats).
    if (is_active && !m_time_events_reported_active) {
        m_time_events_reported_active = true;
        m_time_events_begin_time = begin_time;
        m_time_events_iteration = isinf(simple_duration()) ? 0 : floor((current_time - *begin_time) / simple_duration());
        queue_time_event(HTML::EventNames::beginEvent);
        return;
    }

    // NB: An interval that began and ended since the time events were last checked raises both events.
    if (!is_active && begin_time.has_value() && begin_time != m_time_events_begin_time && last_checked_time.has_value()
        && *begin_time >= *last_checked_time && *begin_time <= current_time) {
        m_time_events_begin_time = begin_time;
        queue_time_event(HTML::EventNames::beginEvent);
        queue_time_event(HTML::EventNames::endEvent);
        return;
    }

    // repeatEvent
    //     This event is raised when an element local timeline repeats. It will be raised each time the element
    //     repeats, after the first iteration. The event provides a numerical indication of which repeat iteration is
    //     beginning. The value is a 0-based integer, but the repeat event is not raised for the first iteration and
    //     so the observed values of the detail attribute will be >= 1.
    if (is_active && !isinf(simple_duration())) {
        auto iteration = floor((current_time - *begin_time) / simple_duration());
        auto previous_iteration = exchange(m_time_events_iteration, iteration);
        if (iteration > previous_iteration)
            queue_time_event(HTML::EventNames::repeatEvent, static_cast<i32>(min(iteration, static_cast<double>(NumericLimits<i32>::max()))));
    }
}

}
