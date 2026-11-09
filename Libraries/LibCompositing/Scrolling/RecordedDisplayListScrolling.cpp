/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Scrolling/RecordedDisplayListScrolling.h>
#include <LibIPC/Encoded.h>

namespace Compositing {

using EncodedAsyncScrollingMetadata = IPC::Encoded<Optional<DisplayList::AsyncScrollingMetadata>>;

static void receive_json(void* receiver, ReadonlyBytes bytes)
{
    *static_cast<String*>(receiver) = MUST(String::from_utf8(bytes));
}

String async_scrolling_state_as_json(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    String json;
    EncodedAsyncScrollingMetadata async_scrolling_metadata { display_list.async_scrolling_metadata() };
    auto display_list_for_scroll_logic = display_list.for_scroll_logic(async_scrolling_metadata.bytes());
    RustFFI::recorded_display_list_async_scrolling_state_json(&display_list_for_scroll_logic, visual_context_tree.rust_handle(), &json, receive_json);
    return json;
}

String empty_async_scrolling_state_as_json()
{
    String json;
    RustFFI::recorded_display_list_async_scrolling_state_json(nullptr, nullptr, &json, receive_json);
    return json;
}

FirstWheelStepRouteForTesting route_first_wheel_step_through_recorded_display_list(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree, ReadonlyBytes encoded_scroll_state, u64 newest_wheel_event_listener_state_generation, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    EncodedAsyncScrollingMetadata async_scrolling_metadata { display_list.async_scrolling_metadata() };
    auto display_list_for_scroll_logic = display_list.for_scroll_logic(async_scrolling_metadata.bytes());
    auto route = RustFFI::recorded_display_list_route_first_wheel_step(&display_list_for_scroll_logic, visual_context_tree.rust_handle(), encoded_scroll_state, newest_wheel_event_listener_state_generation, position, delta_in_css_pixels);

    FirstWheelStepRouteForTesting result { .route = { route.decision }, .document_id = {} };
    if (route.has_scroller) {
        result.route.scroller = Web::AsyncScrollNodeStableID {
            .node_id = Web::UniqueNodeID { route.scroller_node_id },
            .kind = route.scroller_kind,
            .pseudo_element_type = route.scroller_pseudo_element_type,
        };
    }
    if (route.has_document_id)
        result.document_id = Web::UniqueNodeID { route.document_id };
    return result;
}

bool recorded_display_list_blocks_wheel_event_at(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree, ReadonlyBytes encoded_scroll_state, Gfx::FloatPoint position)
{
    EncodedAsyncScrollingMetadata async_scrolling_metadata { display_list.async_scrolling_metadata() };
    auto display_list_for_scroll_logic = display_list.for_scroll_logic(async_scrolling_metadata.bytes());
    return RustFFI::recorded_display_list_blocks_wheel_event_at(&display_list_for_scroll_logic, visual_context_tree.rust_handle(), encoded_scroll_state, position);
}

}
