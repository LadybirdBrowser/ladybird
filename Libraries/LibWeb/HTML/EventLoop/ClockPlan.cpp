/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventLoop/ClockPlan.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::HTML {

// The next time, in the local time of `effect`, at which the main thread has events of it to send: where its phase
// changes if a listener hears its events there, or where its next iteration starts if one hears its iteration events.
// Infinity where it has none.
static Optional<double> next_event_in_local_time(Animations::KeyframeEffect const& effect, double local_time)
{
    if (effect.start_delay().type != Animations::TimeValue::Type::Milliseconds
        || effect.iteration_duration().type != Animations::TimeValue::Type::Milliseconds
        || effect.active_duration().type != Animations::TimeValue::Type::Milliseconds)
        return {};
    auto start_delay = effect.start_delay().value;
    auto iteration_duration = effect.iteration_duration().value;
    auto active_end = start_delay + effect.active_duration().value;
    if (local_time >= active_end)
        return {};
    auto next_event = AK::Infinity<double>;
    if (effect.phase_events_are_heard())
        next_event = local_time < start_delay ? start_delay : active_end;
    if (iteration_duration > 0 && effect.css_animation_iteration_events_are_heard()) {
        auto next_iteration_start = start_delay + max(1.0, floor((local_time - start_delay) / iteration_duration) + 1) * iteration_duration;
        next_event = min(next_event, min(next_iteration_start, active_end));
    }
    return next_event;
}

// Whether the keyframes of `effect` animate a property a clock tick cannot sample: a custom property, or one that changes
// the layout tree's shape or what the main thread observes of an element's box.
static bool animates_what_a_tick_cannot(Animations::KeyframeEffect const& effect)
{
    auto const* key_frame_set = effect.key_frame_set();
    if (!key_frame_set)
        return false;
    for (auto const& keyframe : key_frame_set->keyframes_by_key) {
        for (auto const& [property, value] : keyframe.properties) {
            if (property.is_custom_property() || first_is_one_of(property.id(), CSS::PropertyID::Display, CSS::PropertyID::Visibility, CSS::PropertyID::ContentVisibility))
                return true;
        }
    }
    return false;
}

bool seal_clock_plan(DOM::Document& document, bool may_plan)
{
    auto* arena = document.layout_node_arena_if_created();
    if (!arena)
        return false;
    if (may_plan) {
        // The plan reads the boxes of the elements it names and seals its round from the document's layout.
        Layout::ForcedReadScope read { document };
        // The elements whose running animations a tick samples, the timestamp of the next event of the document's
        // animations, at which the main thread takes over again, and the timestamp at which the sampled animations have
        // all ended, after which a tick has nothing left to move.
        Vector<u32> elements;
        double deadline = AK::Infinity<double>;
        double last_end = -AK::Infinity<double>;
        auto plan = [&] {
            if (!document.is_fully_active() || document.hidden() || !document.window())
                return false;
            auto timeline = document.timeline();
            auto timeline_time = timeline->current_time();
            if (!timeline_time.has_value() || timeline_time->type != Animations::TimeValue::Type::Milliseconds)
                return false;
            for (auto const& associated_timeline : document.associated_animation_timelines()) {
                for (auto& animation : associated_timeline->associated_animations()) {
                    if (animation.play_state() != Bindings::AnimationPlayState::Running)
                        continue;
                    // A tick moves the document's timeline alone, at the rate it runs. A scroll timeline holds still
                    // while the scroll offsets the host laid out do, as it does while the host runs a task, and so do
                    // its animations.
                    if (animation.pending() || !(animation.playback_rate() > 0))
                        return false;
                    if (associated_timeline.ptr() != timeline.ptr()) {
                        if (is<Animations::ScrollTimeline>(*associated_timeline))
                            continue;
                        return false;
                    }
                    auto effect = animation.effect();
                    if (!effect || !is<Animations::KeyframeEffect>(*effect))
                        return false;
                    auto& keyframe_effect = static_cast<Animations::KeyframeEffect&>(*effect);
                    auto target = keyframe_effect.target();
                    if (!target || &target->document() != &document || !target->is_connected())
                        return false;
                    auto local_time = keyframe_effect.local_time();
                    if (!local_time.has_value() || local_time->type != Animations::TimeValue::Type::Milliseconds)
                        return false;
                    auto next_event = next_event_in_local_time(keyframe_effect, local_time->value);
                    if (!next_event.has_value())
                        return false;
                    deadline = min(deadline, timeline_time->value + (*next_event - local_time->value) / animation.playback_rate());
                    // What the compositor runs, or what the main thread does not sample per frame either, a tick does not
                    // sample: the lease only stops at its events.
                    if (keyframe_effect.is_compositor_driven() || keyframe_effect.is_compositor_replaced() || keyframe_effect.can_skip_per_frame_style_update())
                        continue;
                    // The root element and the body paint the background the canvas may take over, which only the host
                    // resolves.
                    auto const* layout_node = target->unsafe_layout_node(read);
                    if (keyframe_effect.pseudo_element_type().has_value() || target->namespace_uri() != Namespace::HTML
                        || target.ptr() == document.document_element() || target.ptr() == document.body()
                        || !layout_node || !Painting::has_committed_box(*layout_node) || animates_what_a_tick_cannot(keyframe_effect))
                        return false;
                    auto active_end = keyframe_effect.start_delay().value + keyframe_effect.active_duration().value;
                    last_end = max(last_end, timeline_time->value + (active_end - local_time->value) / animation.playback_rate());
                    auto element = target->style_node_id().value();
                    if (!elements.contains_slow(element))
                        elements.append(element);
                }
            }
            return !elements.is_empty() && deadline > timeline_time->value;
        };
        if (plan()) {
            auto time_origin = document.relevant_settings_object().time_origin();
            Layout::RustFFI::render_state_seal_clock_plan(arena->host(), read, elements.data(), elements.size(), time_origin, deadline, last_end);
            return true;
        }
    }
    Layout::RustFFI::document_host_drop_clock_plan(arena->host());
    return false;
}

}
