/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MarkerCollector.h>
#include <LibCore/Markers.h>
#include <LibTest/TestCase.h>

TEST_CASE(collector_lifetime_toggles_active_flag_and_pointer)
{
    EXPECT(!Core::g_marker_collector_active);
    EXPECT(Core::g_marker_collector == nullptr);
    {
        Core::MarkerCollector collector;
        EXPECT(Core::g_marker_collector_active);
        EXPECT(Core::g_marker_collector == &collector);
    }
    EXPECT(!Core::g_marker_collector_active);
    EXPECT(Core::g_marker_collector == nullptr);
}

TEST_CASE(each_marker_macro_emits_with_its_phase)
{
    Core::MarkerCollector collector;

    MARKER_INSTANT("a"sv, "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_START_TIME(start);
    MARKER_INTERVAL("b"sv, "Text"sv, Core::MarkerCategory::Other, start, {});
    MARKER_INTERVAL_START("c"sv, "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_INTERVAL_END("d"sv, "Text"sv, Core::MarkerCategory::Other, {});
    {
        MARKER_SCOPE("e"sv, "Text"sv, Core::MarkerCategory::Other);
    }

    auto markers = collector.take_markers_snapshot();
    EXPECT_EQ(markers.size(), 5u);
    EXPECT_EQ(markers[0].phase, Core::MarkerPhase::Instant);
    EXPECT_EQ(markers[1].phase, Core::MarkerPhase::Interval);
    EXPECT_EQ(markers[2].phase, Core::MarkerPhase::IntervalStart);
    EXPECT_EQ(markers[3].phase, Core::MarkerPhase::IntervalEnd);
    EXPECT_EQ(markers[4].phase, Core::MarkerPhase::Interval);
}

TEST_CASE(marker_scope_only_emits_after_destruction)
{
    Core::MarkerCollector collector;
    {
        MARKER_SCOPE("s"sv, "Text"sv, Core::MarkerCategory::Other);
        EXPECT_EQ(collector.take_markers_snapshot().size(), 0u);
    }
    EXPECT_EQ(collector.take_markers_snapshot().size(), 1u);
}

TEST_CASE(marker_scope_fields_attaches_fields)
{
    Core::MarkerCollector collector;
    {
        MARKER_SCOPE_FIELDS("s"sv, "Text"sv, Core::MarkerCategory::Other,
            { { "n"sv, static_cast<i64>(42) } });
    }
    auto markers = collector.take_markers_snapshot();
    EXPECT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].fields.size(), 1u);
    EXPECT_EQ(markers[0].fields[0].value.get<i64>(), 42);
}

TEST_CASE(marker_tid_is_populated)
{
    Core::MarkerCollector collector;
    MARKER_INSTANT("x"sv, "Text"sv, Core::MarkerCategory::Other, {});
    EXPECT(collector.take_markers_snapshot()[0].tid != 0);
}

TEST_CASE(clear_drops_all_markers)
{
    Core::MarkerCollector collector;
    MARKER_INSTANT("a"sv, "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_INSTANT("b"sv, "Text"sv, Core::MarkerCategory::Other, {});
    EXPECT_EQ(collector.take_markers_snapshot().size(), 2u);
    collector.clear();
    EXPECT_EQ(collector.take_markers_snapshot().size(), 0u);
}

TEST_CASE(inactive_gated_macros_do_not_evaluate_args)
{
    EXPECT(!Core::g_marker_collector_active);

    int calls = 0;
    auto bomb = [&]() -> StringView {
        ++calls;
        return ""sv;
    };

    MARKER_INSTANT(bomb(), "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_INTERVAL_START(bomb(), "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_INTERVAL_END(bomb(), "Text"sv, Core::MarkerCategory::Other, {});
    MARKER_START_TIME(start);
    MARKER_INTERVAL(bomb(), "Text"sv, Core::MarkerCategory::Other, start, {});

    EXPECT_EQ(calls, 0);
}

TEST_CASE(inactive_marker_scope_fields_lambda_is_not_invoked)
{
    EXPECT(!Core::g_marker_collector_active);

    int calls = 0;
    auto bomb = [&]() -> StringView {
        ++calls;
        return ""sv;
    };

    {
        MARKER_SCOPE_FIELDS("s"sv, "Text"sv, Core::MarkerCategory::Other,
            { { bomb(), ""sv } });
    }
    EXPECT_EQ(calls, 0);
}
