/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventLoop/ClockPlan.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
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

// The local times of `effect`, around `local_time` on a scroll timeline, from which and up to which the main thread
// has no events of it to send: where its phase changes if a listener hears its events there, or where its current
// iteration changes if one hears its iteration events.
struct LocalInterval {
    double start { -AK::Infinity<double> };
    double end { AK::Infinity<double> };
};

static Optional<LocalInterval> eventless_interval_in_local_time(Animations::KeyframeEffect const& effect, double local_time)
{
    if (effect.start_delay().type != Animations::TimeValue::Type::Percentage
        || effect.iteration_duration().type != Animations::TimeValue::Type::Percentage
        || effect.active_duration().type != Animations::TimeValue::Type::Percentage)
        return {};
    auto start_delay = effect.start_delay().value;
    auto iteration_duration = effect.iteration_duration().value;
    auto active_end = start_delay + effect.active_duration().value;
    LocalInterval interval;
    if (effect.phase_events_are_heard()) {
        if (local_time < start_delay)
            interval.end = start_delay;
        else if (local_time < active_end)
            interval = { start_delay, active_end };
        else
            interval.start = active_end;
    }
    if (iteration_duration > 0 && effect.css_animation_iteration_events_are_heard() && local_time >= start_delay && local_time < active_end) {
        auto iteration_start = start_delay + floor((local_time - start_delay) / iteration_duration) * iteration_duration;
        interval.start = max(interval.start, iteration_start);
        interval.end = min(interval.end, iteration_start + iteration_duration);
    }
    return interval;
}

// Adds the scroll timeline `timeline` that `effect` runs on, at `local_time`, to `scroll_timelines`, narrowed to the
// progress around its current time over which the effect has no events for the main thread to send. Answers whether a
// tick may sample it.
static bool plan_scroll_timeline(Vector<Layout::RustFFI::FfiPlannedScrollTimeline>& scroll_timelines, Animations::ScrollTimeline const& timeline, Animations::KeyframeEffect const& effect, Optional<Animations::TimeValue> local_time, double playback_rate)
{
    auto const& scroller = timeline.followed_scroller();
    auto progress = timeline.current_time();
    if (!scroller.has_value() || !progress.has_value() || progress->type != Animations::TimeValue::Type::Percentage
        || !local_time.has_value() || local_time->type != Animations::TimeValue::Type::Percentage)
        return false;
    auto interval = eventless_interval_in_local_time(effect, local_time->value);
    if (!interval.has_value())
        return false;
    auto scroller_id = scroller->scroll_node.node_id.value();
    auto index = scroll_timelines.find_first_index_if([&](auto const& planned) { return planned.scroller == scroller_id && planned.vertical == scroller->vertical; });
    if (!index.has_value()) {
        index = scroll_timelines.size();
        scroll_timelines.append({
            .scroller = scroller_id,
            .vertical = scroller->vertical,
            .max_scroll_offset = scroller->max_scroll_offset,
            .progress = progress->value,
            .progress_start = -AK::Infinity<double>,
            .progress_end = AK::Infinity<double>,
        });
    }
    auto& planned = scroll_timelines[*index];
    planned.progress_start = max(planned.progress_start, progress->value - (local_time->value - interval->start) / playback_rate);
    planned.progress_end = min(planned.progress_end, progress->value + (interval->end - local_time->value) / playback_rate);
    return true;
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

// Whether a tick can sample the animation of `effect` on `target`.
static bool tick_can_sample(DOM::Document const& document, Animations::KeyframeEffect const& effect, DOM::Element const& target, Layout::BegunRead const& read)
{
    // The root element and the body paint the background the canvas may take over, which only the host resolves.
    auto const* layout_node = target.unsafe_layout_node(read);
    return !effect.pseudo_element_type().has_value() && target.namespace_uri() == Namespace::HTML
        && &target != document.document_element() && &target != document.body()
        && layout_node && Painting::has_committed_box(*layout_node) && !animates_what_a_tick_cannot(effect);
}

// An animation of a scroll timeline that finished runs again as the scroll goes back.
static bool runs_for_clock_plan(Animations::Animation const& animation, bool on_scroll_timeline)
{
    auto play_state = animation.play_state();
    return play_state == Bindings::AnimationPlayState::Running || (on_scroll_timeline && play_state == Bindings::AnimationPlayState::Finished);
}

// The compositor runs these on its own, so the render clock does not sample them.
static bool runs_on_compositor(Animations::KeyframeEffect const& effect)
{
    return effect.is_compositor_driven() || effect.is_compositor_replaced();
}

bool runs_animations_for_clock_plan(DOM::Document const& document)
{
    return any_of(document.associated_animation_timelines(), [](auto const& timeline) {
        bool const on_scroll_timeline = is<Animations::ScrollTimeline>(*timeline);
        return any_of(timeline->associated_animations(), [&](auto const& animation) {
            auto const* effect = as_if<Animations::KeyframeEffect>(animation.effect().ptr());
            return runs_for_clock_plan(animation, on_scroll_timeline) && effect && !runs_on_compositor(*effect);
        });
    });
}

// Whether the render clock may hover what is under the pointer while a task runs. LIBWEB_HOVER_LANE=0 turns it off.
bool hover_lane_is_enabled()
{
    static bool const enabled = [] {
        auto value = getenv("LIBWEB_HOVER_LANE");
        return !value || StringView { value, strlen(value) } != "0"sv;
    }();
    return enabled;
}

// The page's cursor, as a hover of the render clock asks it to show the cursor of what it hovers, on the StyleLayout
// thread.
static Layout::RustFFI::FfiPageCursor ffi_page_cursor(PageCursor& cursor)
{
    return {
        .cursor = &cursor,
        .retain = [](void const* cursor) { static_cast<PageCursor const*>(cursor)->ref(); },
        .release = [](void const* cursor) { static_cast<PageCursor const*>(cursor)->unref(); },
        .request = [](void const* cursor, u8 css_cursor) {
            auto& page_cursor = const_cast<PageCursor&>(*static_cast<PageCursor const*>(cursor));
            page_cursor.request(css_to_gfx_cursor(static_cast<CSS::CursorPredefined>(css_cursor))); },
    };
}

bool seal_clock_plan(DOM::Document& document, bool may_plan, bool may_animate)
{
    auto* arena = document.layout_node_arena_if_created();
    if (!arena)
        return false;
    if (may_plan) {
        // The plan reads the boxes of the elements it names and seals its round from the document's layout.
        Layout::ForcedReadScope read { document };
        // The elements whose running animations a tick samples, the timestamp of the next event of the document's
        // animations, at which the main thread takes over again, the timestamp at which the sampled animations of the
        // document timeline have all ended, after which only a scroll moves anything, and the scroll timelines a tick
        // samples where the compositor has scrolled to.
        Vector<u32> elements;
        double deadline = AK::Infinity<double>;
        double last_end = -AK::Infinity<double>;
        Vector<Layout::RustFFI::FfiPlannedScrollTimeline> scroll_timelines;
        Vector<u32> held_elements;
        auto plan = [&] {
            if (!document.is_fully_active() || document.hidden() || !document.window())
                return false;
            auto timeline = document.timeline();
            auto timeline_time = timeline->current_time();
            if (!timeline_time.has_value() || timeline_time->type != Animations::TimeValue::Type::Milliseconds)
                return false;
            auto plan_animation = [&](Animations::Animation const& animation, Animations::ScrollTimeline const* scroll_timeline, Animations::KeyframeEffect const& effect, DOM::Element const& target) {
                // A tick moves the document's timeline, at the rate it runs, and the scroll timelines, to where the
                // compositor has scrolled.
                if (animation.pending() || !(animation.playback_rate() > 0))
                    return false;
                // A scroll timeline with nothing to scroll holds still, and so do its animations.
                if (scroll_timeline && !scroll_timeline->followed_scroller().has_value())
                    return false;
                // What the compositor runs, or what the main thread does not sample per frame either, a tick does not
                // sample: the lease only stops at its events.
                bool const tick_samples = !runs_on_compositor(effect) && !effect.can_skip_per_frame_style_update();
                if (tick_samples && !tick_can_sample(document, effect, target, read))
                    return false;
                auto local_time = effect.local_time();
                if (scroll_timeline) {
                    if (!plan_scroll_timeline(scroll_timelines, *scroll_timeline, effect, local_time, animation.playback_rate()))
                        return false;
                } else {
                    if (!local_time.has_value() || local_time->type != Animations::TimeValue::Type::Milliseconds)
                        return false;
                    auto next_event = next_event_in_local_time(effect, local_time->value);
                    if (!next_event.has_value())
                        return false;
                    deadline = min(deadline, timeline_time->value + (*next_event - local_time->value) / animation.playback_rate());
                }
                if (!tick_samples)
                    return true;
                if (!scroll_timeline) {
                    auto active_end = effect.start_delay().value + effect.active_duration().value;
                    last_end = max(last_end, timeline_time->value + (active_end - local_time->value) / animation.playback_rate());
                }
                auto element = target.style_node_id().value();
                if (!elements.contains_slow(element))
                    elements.append(element);
                return true;
            };
            for (auto const& associated_timeline : document.associated_animation_timelines()) {
                auto const* scroll_timeline = as_if<Animations::ScrollTimeline>(*associated_timeline);
                for (auto& animation : associated_timeline->associated_animations()) {
                    if (!runs_for_clock_plan(animation, scroll_timeline != nullptr))
                        continue;
                    if (associated_timeline.ptr() != timeline.ptr() && !scroll_timeline)
                        return false;
                    auto const* effect = as_if<Animations::KeyframeEffect>(animation.effect().ptr());
                    auto target = effect ? effect->target() : nullptr;
                    bool const animates_element = target && &target->document() == &document && target->is_connected();
                    if (animates_element && plan_animation(animation, scroll_timeline, *effect, *target))
                        continue;
                    if (!scroll_timeline)
                        return false;
                    if (animates_element && !effect->pseudo_element_type().has_value())
                        held_elements.append(target->style_node_id().value());
                }
            }
            if (any_of(held_elements, [&](auto element) { return elements.contains_slow(element); }))
                return false;
            return !elements.is_empty() && deadline > timeline_time->value;
        };
        bool const animates = may_animate && plan();
        if (!animates) {
            elements.clear();
            deadline = AK::Infinity<double>;
            last_end = -AK::Infinity<double>;
        }
        // The lease's ticks may also follow the pointer, and hover what is under it while a task runs.
        Optional<Layout::RustFFI::FfiHoverPlanInputs> hover;
        Span<Gfx::FloatPoint const> scroll_offsets;
        if (hover_lane_is_enabled() && document.is_fully_active() && !document.hidden() && document.window()) {
            scroll_offsets = document.scroll_state_snapshot().device_offsets();
            hover = Layout::RustFFI::FfiHoverPlanInputs {
                .device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel(),
                .scroll_offsets = scroll_offsets.data(),
                .scroll_offset_count = scroll_offsets.size(),
                .chrome_metrics = document.page().chrome_metrics(),
                .may_have_scroll_snap_areas = document.may_have_scroll_snap_areas(),
                .page_cursor = ffi_page_cursor(document.page().cursor()),
            };
        }
        if (animates || hover.has_value()) {
            auto time_origin = document.relevant_settings_object().time_origin();
            Layout::RustFFI::render_state_seal_clock_plan(arena->host(), read, elements.data(), elements.size(), time_origin, deadline, last_end, scroll_timelines.data(), scroll_timelines.size(), hover.has_value() ? &*hover : nullptr);
            return true;
        }
    }
    Layout::RustFFI::document_host_drop_clock_plan(arena->host());
    return false;
}

}
