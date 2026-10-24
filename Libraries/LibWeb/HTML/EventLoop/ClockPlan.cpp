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
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleEngineEffectTiming.h>
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

// Whether the timing of `effect` is in time values of `type` throughout: milliseconds on the document timeline, and
// percentages of progress on a scroll timeline.
static bool timing_is_in(Animations::KeyframeEffect const& effect, Animations::TimeValue::Type type)
{
    return effect.start_delay().type == type && effect.iteration_duration().type == type && effect.active_duration().type == type;
}

// Adds the scroll timeline `timeline` that `effect` runs on, at `local_time`, to `scroll_timelines`. Answers whether a
// tick may sample it.
static bool plan_scroll_timeline(Vector<Layout::RustFFI::FfiPlannedScrollTimeline>& scroll_timelines, Animations::ScrollTimeline const& timeline, Animations::KeyframeEffect const& effect, Optional<Animations::TimeValue> local_time)
{
    auto const& scroller = timeline.followed_scroller();
    auto progress = timeline.current_time();
    if (!scroller.has_value() || !progress.has_value() || progress->type != Animations::TimeValue::Type::Percentage
        || !local_time.has_value() || local_time->type != Animations::TimeValue::Type::Percentage
        || !timing_is_in(effect, Animations::TimeValue::Type::Percentage))
        return false;
    auto scroller_id = scroller->scroll_node.node_id.value();
    if (!scroll_timelines.find_first_index_if([&](auto const& planned) { return planned.scroller == scroller_id && planned.vertical == scroller->vertical; }).has_value()) {
        scroll_timelines.append({
            .scroller = scroller_id,
            .vertical = scroller->vertical,
            .max_scroll_offset = scroller->max_scroll_offset,
            .progress = progress->value,
        });
    }
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

// Whether the keyframes of `effect` animate a property that the children of its target inherit.
static bool animates_an_inherited_property(Animations::KeyframeEffect const& effect)
{
    auto const* key_frame_set = effect.key_frame_set();
    if (!key_frame_set)
        return false;
    for (auto const& keyframe : key_frame_set->keyframes_by_key) {
        for (auto const& [property, value] : keyframe.properties) {
            if (property.is_custom_property() || CSS::is_inherited_property(property.id()))
                return true;
        }
    }
    return false;
}

// Whether no box shows anything of the animation of `effect` on `target`: the target has no box, as under
// `display: none`, or it is `display: contents`, and the effect animates neither one of its pseudo-elements nor what its
// children inherit, or those have no boxes either.
static bool no_box_shows(Animations::KeyframeEffect const& effect, DOM::Element const& target, Layout::BegunRead const& read)
{
    if (target.unsafe_layout_node(read))
        return false;
    if (!target.has_display_contents() || (!effect.pseudo_element_type().has_value() && !animates_an_inherited_property(effect)))
        return true;
    // The pseudo-elements and children of a `display: contents` element are laid out where the nearest element above it
    // with a box lays out its own.
    for (auto const* ancestor = target.flat_tree_parent_element(); ancestor; ancestor = ancestor->flat_tree_parent_element()) {
        if (ancestor->unsafe_layout_node(read))
            return false;
        if (!ancestor->has_display_contents())
            return true;
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

bool seal_clock_plan(DOM::Document& document, bool may_plan, u64 update_serial)
{
    auto* arena = document.layout_node_arena_if_created();
    if (!arena)
        return false;
    if (may_plan) {
        // The plan reads the boxes of the elements it names and seals its round from the document's layout.
        Layout::ForcedReadScope read { document };
        // The elements whose running animations a tick samples, the timestamp at which the sampled animations of the
        // document timeline have all ended, after which only a scroll moves anything, and the scroll timelines a tick
        // samples where the compositor has scrolled to. The ticks go on past the events of the animations, which the
        // main thread sends as it gets to them.
        Vector<u32> elements;
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
                // compositor has scrolled. What the compositor runs already runs there while its play is pending.
                // Anything else holds still while its play is pending, until the host makes it ready, as it does in
                // the boxes of the host's frame: the ticks sample nothing of its element.
                if (animation.pending() && !runs_on_compositor(effect)) {
                    if (!effect.pseudo_element_type().has_value())
                        held_elements.append(target.style_node_id().value());
                    return true;
                }
                if (!(animation.playback_rate() > 0))
                    return false;
                // A scroll timeline with nothing to scroll holds still, and so do its animations.
                if (scroll_timeline && !scroll_timeline->followed_scroller().has_value())
                    return false;
                // What the compositor runs, what the main thread does not sample per frame either, or what no box shows,
                // a tick does not sample.
                bool const tick_samples = !runs_on_compositor(effect) && !effect.can_skip_per_frame_style_update() && !no_box_shows(effect, target, read);
                if (tick_samples && !tick_can_sample(document, effect, target, read))
                    return false;
                auto local_time = effect.local_time();
                if (scroll_timeline) {
                    if (!plan_scroll_timeline(scroll_timelines, *scroll_timeline, effect, local_time))
                        return false;
                } else if (!local_time.has_value() || local_time->type != Animations::TimeValue::Type::Milliseconds
                    || !timing_is_in(effect, Animations::TimeValue::Type::Milliseconds)
                    || local_time->value >= effect.start_delay().value + effect.active_duration().value) {
                    return false;
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
                    auto const* effect = as_if<Animations::KeyframeEffect>(animation.effect().ptr());
                    auto target = effect ? effect->target() : nullptr;
                    if (!target || &target->document() != &document || !target->is_connected())
                        continue;
                    // A tick moves the document's timeline and the scroll timelines alone.
                    bool const on_a_ticked_timeline = associated_timeline.ptr() == timeline.ptr() || scroll_timeline;
                    if (on_a_ticked_timeline && plan_animation(animation, scroll_timeline, *effect, *target))
                        continue;
                    // The boxes of an element with an animation a tick cannot sample show what the host's frame shows
                    // of it, while the ticks sample the others on.
                    if (!effect->pseudo_element_type().has_value())
                        held_elements.append(target->style_node_id().value());
                }
            }
            elements.remove_all_matching([&](auto element) { return held_elements.contains_slow(element); });
            return !elements.is_empty();
        };
        bool const animates = plan();
        if (!animates) {
            elements.clear();
            last_end = -AK::Infinity<double>;
        }
        // The lane's ticks may also follow the pointer, and hover what is under it while a task runs.
        Optional<Layout::RustFFI::FfiHoverPlanInputs> hover;
        Span<Gfx::FloatPoint const> scroll_offsets;
        Vector<u32> effect_timing_nodes;
        Vector<u64> effect_timing_identities;
        Vector<CSS::ComputedValuesFFI::FfiEffectTiming> effect_timings;
        if (hover_lane_is_enabled() && document.is_fully_active() && !document.hidden() && document.window()) {
            // A hover decides over the transitions the host runs as they run, where the host runs them without
            // sampling them, which leaves the timing the engine holds of them behind.
            for (auto const& associated_timeline : document.associated_animation_timelines()) {
                for (auto& animation : associated_timeline->associated_animations()) {
                    auto const* effect = as_if<Animations::KeyframeEffect>(animation.effect().ptr());
                    auto target = effect ? effect->target() : nullptr;
                    if (!effect || !target || effect->pseudo_element_type().has_value() || !target->style_node_id()
                        || !effect->can_skip_per_frame_style_update())
                        continue;
                    auto timing = CSS::style_engine_effect_timing(*effect, animation);
                    // The compositor runs a transition whose play is still pending from the time of the style change
                    // event that started it.
                    if (auto const* transition = as_if<CSS::CSSTransition>(animation); transition && animation.pending()
                        && !animation.start_time().has_value() && effect->is_compositor_driven()) {
                        timing.has_start_time = true;
                        timing.start_time = transition->transition_start_time() - effect->start_delay().value;
                        timing.has_hold_time = false;
                    }
                    effect_timing_nodes.append(target->style_node_id().value());
                    effect_timing_identities.append(effect->animation_preparation_identity());
                    effect_timings.append(timing);
                }
            }
            scroll_offsets = document.scroll_state_snapshot().device_offsets();
            hover = Layout::RustFFI::FfiHoverPlanInputs {
                .device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel(),
                .scroll_offsets = scroll_offsets.data(),
                .scroll_offset_count = scroll_offsets.size(),
                .chrome_metrics = document.page().chrome_metrics(document),
                .page_cursor = ffi_page_cursor(document.page().cursor()),
                .effect_timing_nodes = effect_timing_nodes.data(),
                .effect_timing_identities = effect_timing_identities.data(),
                .effect_timings = effect_timings.data(),
                .effect_timing_count = effect_timings.size(),
            };
        }
        if (animates || hover.has_value()) {
            auto time_origin = document.relevant_settings_object().time_origin();
            Layout::RustFFI::render_state_seal_clock_plan(arena->host(), read, elements.data(), elements.size(), time_origin, last_end, scroll_timelines.data(), scroll_timelines.size(), hover.has_value() ? &*hover : nullptr, update_serial);
            return true;
        }
    }
    Layout::RustFFI::document_host_drop_clock_plan(arena->host(), update_serial);
    return false;
}

}
