/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCore/System.h>
#include <LibJS/GeckoProfileWriter.h>
#include <LibJS/Profiler.h>

namespace JS {

// Gecko Profile Format version 34
// https://github.com/firefox-devtools/profiler/blob/main/src/app-logic/constants.ts
String write_gecko_profile(Profiler const& profiler)
{
    // Categories (GeckoProfileFullMeta.categories)
    JsonObject js_category;
    js_category.set("name"sv, "JavaScript"sv);
    js_category.set("color"sv, "yellow"sv);
    JsonArray subcategories;
    subcategories.must_append("Other"sv);
    js_category.set("subcategories"sv, move(subcategories));
    JsonArray categories;
    categories.must_append(move(js_category));

    // Meta (GeckoProfileFullMeta)
    // interval: ms between samples; use 1ms as the nominal value for safe-point-only profilers
    // (interval_us == 0) since the spec expects a positive number.
    double interval_ms = profiler.interval_us() > 0
        ? static_cast<double>(profiler.interval_us()) / 1000.0
        : 1.0;
    JsonObject meta;
    meta.set("version"sv, 34);
    meta.set("interval"sv, interval_ms);
    meta.set("startTime"sv, static_cast<double>(profiler.start_time_epoch_ms()));
    auto stop_ms = profiler.stop_time_epoch_ms();
    if (stop_ms > 0)
        meta.set("shutdownTime"sv, static_cast<double>(stop_ms));
    else
        meta.set("shutdownTime"sv, JsonValue {});
    meta.set("processType"sv, 0);
    meta.set("product"sv, "Ladybird LibJS"sv);
    meta.set("stackwalk"sv, 0);
    meta.set("debug"sv, 0);
    meta.set("gcpoison"sv, 0);
    meta.set("asyncstack"sv, 0);
    meta.set("categories"sv, move(categories));
    meta.set("markerSchema"sv, JsonArray {});

    // Samples (GeckoSamples): [stack, time, responsiveness]
    JsonObject samples_schema;
    samples_schema.set("stack"sv, 0);
    samples_schema.set("time"sv, 1);
    samples_schema.set("responsiveness"sv, 2);
    JsonArray samples_data;
    for (auto const& sample : profiler.samples()) {
        JsonArray row;
        row.must_append(sample.stack_index);
        row.must_append(sample.time_ms);
        row.must_append(0);
        samples_data.must_append(move(row));
    }
    JsonObject samples;
    samples.set("schema"sv, move(samples_schema));
    samples.set("data"sv, move(samples_data));

    // Stack table (GeckoStackTable): [prefix, frame]
    JsonObject stack_schema;
    stack_schema.set("prefix"sv, 0);
    stack_schema.set("frame"sv, 1);
    JsonArray stack_data;
    for (auto const& stack : profiler.stack_table()) {
        JsonArray row;
        if (stack.prefix.has_value())
            row.must_append(stack.prefix.value());
        else
            row.must_append(JsonValue {});
        row.must_append(stack.frame_index);
        stack_data.must_append(move(row));
    }
    JsonObject stack_table;
    stack_table.set("schema"sv, move(stack_schema));
    stack_table.set("data"sv, move(stack_data));

    // Frame table (GeckoFrameTable)
    JsonObject frame_schema;
    frame_schema.set("location"sv, 0);
    frame_schema.set("relevantForJS"sv, 1);
    frame_schema.set("innerWindowID"sv, 2);
    frame_schema.set("implementation"sv, 3);
    frame_schema.set("line"sv, 4);
    frame_schema.set("column"sv, 5);
    frame_schema.set("category"sv, 6);
    frame_schema.set("subcategory"sv, 7);
    JsonArray frame_data;
    for (auto const& frame : profiler.frame_table()) {
        JsonArray row;
        row.must_append(frame.string_index);
        row.must_append(true);
        row.must_append(JsonValue {});
        row.must_append(JsonValue {});
        row.must_append(frame.line);
        row.must_append(frame.column);
        row.must_append(0);
        row.must_append(0);
        frame_data.must_append(move(row));
    }
    JsonObject frame_table;
    frame_table.set("schema"sv, move(frame_schema));
    frame_table.set("data"sv, move(frame_data));

    // String table
    JsonArray string_table;
    for (auto const& str : profiler.string_table())
        string_table.must_append(str);

    // Markers (GeckoMarkers) — empty
    JsonObject markers_schema;
    markers_schema.set("name"sv, 0);
    markers_schema.set("startTime"sv, 1);
    markers_schema.set("endTime"sv, 2);
    markers_schema.set("phase"sv, 3);
    markers_schema.set("category"sv, 4);
    markers_schema.set("data"sv, 5);
    JsonObject markers;
    markers.set("schema"sv, move(markers_schema));
    markers.set("data"sv, JsonArray {});

    // Thread (GeckoThread)
    JsonObject thread;
    thread.set("name"sv, "GeckoMain"sv);
    thread.set("processType"sv, "default"sv);
    thread.set("tid"sv, profiler.os_tid());
    thread.set("pid"sv, Core::System::getpid());
    thread.set("registerTime"sv, static_cast<double>(profiler.start_time_epoch_ms()));
    thread.set("unregisterTime"sv, JsonValue {});
    thread.set("markers"sv, move(markers));
    thread.set("samples"sv, move(samples));
    thread.set("frameTable"sv, move(frame_table));
    thread.set("stackTable"sv, move(stack_table));
    thread.set("stringTable"sv, move(string_table));

    JsonArray threads;
    threads.must_append(move(thread));

    // Root (GeckoProfile)
    JsonObject root;
    root.set("meta"sv, move(meta));
    root.set("libs"sv, JsonArray {});
    root.set("sources"sv, JsonArray {});
    root.set("threads"sv, move(threads));
    root.set("processes"sv, JsonArray {});
    root.set("pausedRanges"sv, JsonArray {});

    return root.serialized();
}

}
