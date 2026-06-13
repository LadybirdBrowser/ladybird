/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibCore/Environment.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

bool history_debug_enabled()
{
    static auto enabled = WEBVIEW_HISTORY_DEBUG || Core::Environment::has("LADYBIRD_SESSION_HISTORY_DEBUG"sv);
    return enabled;
}

static void append_history_log_entry(StringBuilder& builder, Web::HTML::SessionHistoryEntryDescriptor const& entry);

static StringView document_state_resource_type(Variant<Empty, String, Web::HTML::POSTResource> const& resource)
{
    if (resource.has<String>())
        return "string"sv;
    if (resource.has<Web::HTML::POSTResource>())
        return "post"sv;
    return "none"sv;
}

static void append_history_log_nested_histories(StringBuilder& builder, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& nested_histories)
{
    if (nested_histories.is_empty())
        return;

    builder.append(" nested={"sv);
    for (size_t i = 0; i < nested_histories.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);

        builder.appendff("{}=[", nested_histories[i].id);
        for (size_t j = 0; j < nested_histories[i].entries.size(); ++j) {
            if (j != 0)
                builder.append(", "sv);
            append_history_log_entry(builder, nested_histories[i].entries[j]);
        }
        builder.append(']');
    }
    builder.append('}');
}

static void append_history_log_entry(StringBuilder& builder, Web::HTML::SessionHistoryEntryDescriptor const& entry)
{
    builder.appendff("{}:{}", entry.step, entry.url);
    auto resource_type = document_state_resource_type(entry.document_state.resource);
    if (entry.document_state.id != 0
        || entry.document_state.reload_pending
        || !entry.document_state.navigable_target_name.is_empty()
        || resource_type != "none"sv) {
        builder.appendff(" document_state={{id={}", entry.document_state.id);
        if (resource_type != "none"sv)
            builder.appendff(", resource={}", resource_type);
        if (entry.document_state.reload_pending)
            builder.append(", reload_pending=true"sv);
        if (!entry.document_state.navigable_target_name.is_empty())
            builder.appendff(", target_name={}", entry.document_state.navigable_target_name);
        builder.append('}');
    }
    if (entry.scroll_position_data.viewport_scroll_position.has_value()) {
        auto const& viewport_scroll_position = *entry.scroll_position_data.viewport_scroll_position;
        builder.appendff(" scroll={{viewport=({}, {})}}", viewport_scroll_position.x(), viewport_scroll_position.y());
    }
    append_history_log_nested_histories(builder, entry.document_state.nested_histories);
}

static StringView scroll_restoration_mode_to_string(Web::HTML::ScrollRestorationMode mode)
{
    switch (mode) {
    case Web::HTML::ScrollRestorationMode::Auto:
        return "auto"sv;
    case Web::HTML::ScrollRestorationMode::Manual:
        return "manual"sv;
    }
    VERIFY_NOT_REACHED();
}

static JsonObject history_json_scroll_position_data(Web::HTML::SessionHistoryEntryScrollPositionData const& scroll_position_data)
{
    JsonObject serialized;
    if (scroll_position_data.viewport_scroll_position.has_value()) {
        auto const& viewport_scroll_position = *scroll_position_data.viewport_scroll_position;
        JsonArray serialized_viewport_scroll_position;
        serialized_viewport_scroll_position.must_append(viewport_scroll_position.x().to_double());
        serialized_viewport_scroll_position.must_append(viewport_scroll_position.y().to_double());
        serialized.set("viewport"sv, move(serialized_viewport_scroll_position));
    }
    return serialized;
}

static JsonArray history_json_nested_histories(Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& nested_histories)
{
    JsonArray serialized_nested_histories;
    serialized_nested_histories.ensure_capacity(nested_histories.size());
    for (auto const& nested_history : nested_histories) {
        JsonArray serialized_entries;
        serialized_entries.ensure_capacity(nested_history.entries.size());
        for (auto const& entry : nested_history.entries)
            serialized_entries.must_append(history_json_entry(entry));

        JsonObject serialized_nested_history;
        serialized_nested_history.set("id"sv, nested_history.id);
        serialized_nested_history.set("entries"sv, move(serialized_entries));
        serialized_nested_histories.must_append(move(serialized_nested_history));
    }
    return serialized_nested_histories;
}

JsonObject history_json_entry(Web::HTML::SessionHistoryEntryDescriptor const& entry, bool current)
{
    JsonObject serialized;
    serialized.set("step"sv, entry.step);
    serialized.set("url"sv, entry.url.serialize());
    serialized.set("documentStateId"sv, entry.document_state.id);
    serialized.set("resource"sv, document_state_resource_type(entry.document_state.resource));
    serialized.set("reloadPending"sv, entry.document_state.reload_pending);
    serialized.set("targetName"sv, entry.document_state.navigable_target_name);
    serialized.set("scrollRestoration"sv, scroll_restoration_mode_to_string(entry.scroll_restoration_mode));
    serialized.set("scrollPosition"sv, history_json_scroll_position_data(entry.scroll_position_data));
    serialized.set("nestedHistories"sv, history_json_nested_histories(entry.document_state.nested_histories));
    serialized.set("current"sv, current);
    return serialized;
}

JsonArray history_json_entries(TraversableSessionHistory const& history)
{
    return history_json_entries(history.entries(), history.current_top_level_entry_index());
}

JsonArray history_json_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Optional<size_t> current_entry_index)
{
    JsonArray serialized_entries;
    serialized_entries.ensure_capacity(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        serialized_entries.must_append(history_json_entry(entries[i], current_entry_index.has_value() && *current_entry_index == i));
    return serialized_entries;
}

JsonArray history_json_steps(TraversableSessionHistory const& history)
{
    return history_json_steps(history.used_steps(), history.current_used_step_index());
}

JsonArray history_json_steps(Vector<i32> const& steps, Optional<size_t> current_step_index)
{
    JsonArray serialized_steps;
    serialized_steps.ensure_capacity(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        JsonObject serialized_step;
        serialized_step.set("step"sv, steps[i]);
        serialized_step.set("current"sv, current_step_index.has_value() && *current_step_index == i);
        serialized_steps.must_append(move(serialized_step));
    }
    return serialized_steps;
}

ByteString history_log_entries(TraversableSessionHistory const& history)
{
    StringBuilder builder;
    builder.append("entries=["sv);
    for (size_t i = 0; i < history.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);

        auto const* entry = history.entry_at(i);
        VERIFY(entry);
        if (auto const* current_entry = history.current_entry(); current_entry == entry)
            builder.append("*"sv);
        builder.appendff("{}:", i);
        append_history_log_entry(builder, *entry);
    }
    builder.append("] used_steps="sv);
    builder.append(history_log_steps(history.used_steps(), history.current_used_step_index()));
    return builder.to_byte_string();
}

ByteString history_log_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Optional<size_t> current_entry_index)
{
    StringBuilder builder;
    builder.append('[');
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);

        if (current_entry_index.has_value() && *current_entry_index == i)
            builder.append("*"sv);
        builder.appendff("{}:", i);
        append_history_log_entry(builder, entries[i]);
    }
    builder.append(']');
    return builder.to_byte_string();
}

ByteString history_log_steps(Vector<i32> const& steps, Optional<size_t> current_step_index)
{
    StringBuilder builder;
    builder.append('[');
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);

        if (current_step_index.has_value() && *current_step_index == i)
            builder.append("*"sv);
        builder.appendff("{}:{}", i, steps[i]);
    }
    builder.append(']');
    return builder.to_byte_string();
}

}
