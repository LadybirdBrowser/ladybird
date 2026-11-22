/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Math.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventLoop/ClockPlan.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::HTML {

// Whether the keyframes of `effect` animate a property a clock tick cannot sample: a custom property, or one that changes
// the layout tree's shape or what the main thread observes of an element's box.
static bool animates_what_a_tick_cannot(Animations::EffectRef const& effect)
{
    auto const& keyframe_set = effect.keyframe_set();
    return keyframe_set && keyframe_set.facts().animates_what_a_tick_cannot;
}

// Whether the keyframes of `effect` animate a property that the children of its target inherit.
static bool animates_an_inherited_property(Animations::EffectRef const& effect)
{
    auto const& keyframe_set = effect.keyframe_set();
    return keyframe_set && keyframe_set.facts().animates_an_inherited_property;
}

// Whether no box shows anything of the animation of `effect` on `target`: the target has no box, as under
// `display: none`, or it is `display: contents`, and the effect animates neither one of its pseudo-elements nor what its
// children inherit, or those have no boxes either.
static bool no_box_shows(Animations::EffectRef const& effect, DOM::Element const& target, Layout::BegunRead const& read)
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
static bool tick_can_sample(DOM::Document const& document, Animations::EffectRef const& effect, DOM::Element const& target, Layout::BegunRead const& read)
{
    // The root element and the body paint the background the canvas may take over, which only the host resolves.
    auto const* layout_node = target.unsafe_layout_node(read);
    return !effect.pseudo_element_type().has_value() && target.namespace_uri() == Namespace::HTML
        && &target != document.document_element() && &target != document.body()
        && layout_node && Painting::has_committed_box(*layout_node) && !animates_what_a_tick_cannot(effect);
}

// NB: An animation of a scroll timeline that finished runs again as the scroll goes back.
static bool runs_for_clock_plan(Animations::RustFFI::AnimationPlayState play_state, bool on_scroll_timeline)
{
    return play_state == Animations::RustFFI::AnimationPlayState::Running || (on_scroll_timeline && play_state == Animations::RustFFI::AnimationPlayState::Finished);
}

// The compositor runs these on its own, so the render clock does not sample them.
static bool runs_on_compositor(Animations::EffectRef const& effect)
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

// The page's cursor, as a hover of the render clock asks it to show the cursor of what it hovers, on the Render
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

// What tells the document's page the link a hover of the render clock is over, on the Render thread.
static Layout::RustFFI::FfiHoverLinkReporter ffi_hover_link_reporter(DOM::Document& document)
{
    document.update_hover_link_base();
    return {
        .reporter = &document.hover_link_reporter(),
        .retain = [](void const* reporter) { static_cast<HoverLinkReporter const*>(reporter)->ref(); },
        .release = [](void const* reporter) { static_cast<HoverLinkReporter const*>(reporter)->unref(); },
        .is_link = [](void const* reporter, u32 element) { return static_cast<HoverLinkReporter const*>(reporter)->is_link(element); },
        .report = [](void const* reporter, u32 element) { const_cast<HoverLinkReporter&>(*static_cast<HoverLinkReporter const*>(reporter)).report(element); },
        .withdraw = [](void const* reporter) { const_cast<HoverLinkReporter&>(*static_cast<HoverLinkReporter const*>(reporter)).withdraw(); },
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
        // OPTIMIZATION: Hash sets keep a plan over thousands of animated elements from scanning lists for each one.
        HashTable<u32> element_set;
        double last_end = -AK::Infinity<double>;
        Vector<Animations::RustFFI::TimelineCell const*> scroll_timelines;
        HashTable<u32> held_elements;
        auto plan = [&] {
            if (!document.is_fully_active() || document.hidden() || !document.window())
                return false;
            auto timeline = document.timeline();
            auto timeline_time = timeline.current_time();
            if (!timeline_time.has_value() || timeline_time->type != Animations::RustFFI::TimeValueType::Milliseconds)
                return false;
            auto plan_animation = [&](Animations::AnimationRef const& animation, Animations::TimelineView const* scroll_timeline, Animations::EffectRef const& effect, DOM::Element const& target) {
                // NB: The record decides what the plan makes of the animation as far as its timing goes. One whose play
                //     is pending holds still, and the ticks sample nothing of its element.
                auto planned = Animations::RustFFI::animations_animation_clock_plan(&animation.record());
                if (planned.kind == Animations::RustFFI::FfiClockPlanAnimationKind::Held) {
                    if (!effect.pseudo_element_type().has_value())
                        held_elements.set(target.style_node_id().value());
                    return true;
                }
                // NB: Nor does a tick move one it cannot, as one of a scroll timeline with nothing to scroll, or one that a
                //     tick cannot follow.
                if (planned.kind == Animations::RustFFI::FfiClockPlanAnimationKind::NotTicked)
                    return false;
                // What the compositor runs, what the main thread does not sample per frame either, or what no box shows,
                // a tick does not sample.
                bool const tick_samples = !runs_on_compositor(effect) && !effect.can_skip_per_frame_style_update() && !no_box_shows(effect, target, read);
                if (tick_samples && !tick_can_sample(document, effect, target, read))
                    return false;
                // NB: The plan takes the scroll timelines a tick follows as they are when it is sealed.
                if (scroll_timeline && (scroll_timelines.is_empty() || scroll_timelines.last() != &scroll_timeline->record()))
                    scroll_timelines.append(&scroll_timeline->record());
                if (!tick_samples)
                    return true;
                if (!scroll_timeline)
                    last_end = max(last_end, timeline_time->value + (planned.active_end.value - planned.local_time.value) / animation.playback_rate());
                auto element = target.style_node_id().value();
                if (element_set.set(element) == HashSetResult::InsertedNewEntry)
                    elements.append(element);
                return true;
            };
            for (auto const& associated_timeline : document.associated_animation_timelines()) {
                Animations::TimelineView const* scroll_timeline = associated_timeline.is_scroll_timeline() ? &associated_timeline : nullptr;
                // OPTIMIZATION: Every animation the plan runs is one an update of its timeline may change.
                auto associated_animations = Animations::associated_animations_of(associated_timeline);
                for (auto const& animation : Animations::animations_needing_update_of(associated_timeline)) {
                    if (!associated_animations.contains(animation))
                        continue;
                    if (!runs_for_clock_plan(animation.play_state(), scroll_timeline != nullptr))
                        continue;
                    auto effect = animation.effect();
                    auto* target = effect.has_value() ? effect->target() : nullptr;
                    if (!target || &target->document() != &document || !target->is_connected())
                        continue;
                    // A tick moves the document's timeline and the scroll timelines alone.
                    bool const on_a_ticked_timeline = associated_timeline == timeline || scroll_timeline;
                    if (on_a_ticked_timeline && plan_animation(animation, scroll_timeline, *effect, *target))
                        continue;
                    // The boxes of an element with an animation a tick cannot sample show what the host's frame shows
                    // of it, while the ticks sample the others on.
                    if (!effect->pseudo_element_type().has_value())
                        held_elements.set(target->style_node_id().value());
                }
            }
            elements.remove_all_matching([&](auto element) { return held_elements.contains(element); });
            return !elements.is_empty();
        };
        bool const animates = plan();
        if (!animates) {
            elements.clear();
            last_end = -AK::Infinity<double>;
        }
        // The lane's ticks may also follow the pointer, and hover what is under it while a task runs.
        Optional<Layout::RustFFI::FfiHoverPlanInputs> hover;
        Vector<u32> effect_timing_nodes;
        Vector<Animations::RustFFI::AnimationEffectId> effect_timing_identities;
        Vector<Animations::RustFFI::EffectTimingSnapshot> effect_timings;
        if (hover_lane_is_enabled() && document.is_fully_active() && !document.hidden() && document.window()) {
            // A hover decides over the transitions the host runs as they run, where the host runs them without
            // sampling them, which leaves the timing the engine holds of them behind.
            // OPTIMIZATION: An animation whose effect produces nothing leaves the animations associated with its
            //               target, and one an update of its timeline may change is among those its timeline keeps,
            //               so look only at those, not at every animation associated with a timeline. Those of
            //               timelines the document is not associated with, or of none, are left out as before.
            auto const associated_timelines = document.associated_animation_timelines();
            HashTable<Animations::RustFFI::TimelineCell const*> timelines;
            for (auto const& timeline : associated_timelines)
                timelines.set(&timeline.record());
            auto add_effect_timing = [&](Animations::AnimationRef const& animation) {
                auto effect = animation.effect();
                auto* target = effect.has_value() ? effect->target() : nullptr;
                if (!target || effect->pseudo_element_type().has_value() || !target->style_node_id()
                    || !effect->can_skip_per_frame_style_update()
                    || !timelines.contains(Animations::RustFFI::animations_animation_timeline(&animation.record())))
                    return;
                // NB: The record decides the timing the hover runs the animation with.
                effect_timing_nodes.append(target->style_node_id().value());
                effect_timing_identities.append(effect->id());
                effect_timings.append(Animations::RustFFI::animations_animation_hover_timing(&animation.record()));
            };
            auto target_associated_animations = Animations::associated_animations_of(document.document_animations());
            for (auto const& animation : target_associated_animations)
                add_effect_timing(animation);
            // NB: A transition started provisionally runs on its timeline before its target observes it.
            for (auto const& timeline : associated_timelines) {
                auto associated_animations = Animations::associated_animations_of(timeline);
                for (auto const& animation : Animations::animations_needing_update_of(timeline)) {
                    if (associated_animations.contains(animation) && !target_associated_animations.contains(animation))
                        add_effect_timing(animation);
                }
            }
            Optional<Web::EventHandler::SelectionDragForLanes> selection_drag;
            if (auto navigable = document.navigable())
                selection_drag = navigable->event_handler().selection_drag_for_lanes(read);
            auto scrollport = selection_drag.has_value() ? selection_drag->scrollport.value_or(CSSPixelRect {}) : CSSPixelRect {};
            hover = Layout::RustFFI::FfiHoverPlanInputs {
                .device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel(),
                .chrome_metrics = document.page().chrome_metrics(),
                .page_cursor = ffi_page_cursor(document.page().cursor()),
                .link_reporter = ffi_hover_link_reporter(document),
                .effect_timing_nodes = effect_timing_nodes.data(),
                .effect_timing_identities = effect_timing_identities.data(),
                .effect_timings = effect_timings.data(),
                .effect_timing_count = effect_timings.size(),
                .selection_drag_anchor = selection_drag.has_value() ? selection_drag->anchor.value() : 0,
                .selection_drag_anchor_offset = selection_drag.has_value() ? selection_drag->anchor_offset : 0,
                .selection_drag_anchor_child = selection_drag.has_value() ? selection_drag->anchor_child.value() : 0,
                .selection_drag_scrollport = {
                    .x = scrollport.x().to_float(),
                    .y = scrollport.y().to_float(),
                    .width = scrollport.width().to_float(),
                    .height = scrollport.height().to_float(),
                },
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
