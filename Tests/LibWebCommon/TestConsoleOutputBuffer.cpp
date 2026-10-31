/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/WebView/ConsoleOutputBuffer.h>

static WebView::ConsoleOutput log_of(String text)
{
    return {
        .timestamp = UnixDateTime::from_seconds_since_epoch(1),
        .output = WebView::ConsoleLog {
            .level = JS::ConsoleLogLevel::Log,
            .arguments = { move(text) },
            .type = WebView::ConsoleLogType::ConsoleAPI,
            .location = {},
            .stacktrace = {},
        },
    };
}

static Vector<String> texts_of(WebView::ConsoleOutputBuffer const& buffer)
{
    Vector<String> texts;
    buffer.for_each([&](WebView::ConsoleOutput const& output) {
        texts.append(output.output.get<WebView::ConsoleLog>().arguments.first().as_string());
    });
    return texts;
}

TEST_CASE(keeps_messages_in_order)
{
    WebView::ConsoleOutputBuffer buffer;
    buffer.append(log_of("first"_string));
    buffer.append(log_of("second"_string));
    EXPECT_EQ(texts_of(buffer), (Vector { "first"_string, "second"_string }));
}

TEST_CASE(keeps_only_the_latest_messages)
{
    WebView::ConsoleOutputBuffer buffer;
    for (size_t i = 0; i <= WebView::ConsoleOutputBuffer::MAX_MESSAGES; ++i)
        buffer.append(log_of(String::number(i)));

    auto texts = texts_of(buffer);
    EXPECT_EQ(texts.size(), WebView::ConsoleOutputBuffer::MAX_MESSAGES);
    EXPECT_EQ(texts.first(), "1"sv);
    EXPECT_EQ(texts.last(), String::number(WebView::ConsoleOutputBuffer::MAX_MESSAGES));
}

TEST_CASE(drops_older_messages_past_the_size_limit)
{
    WebView::ConsoleOutputBuffer buffer;
    auto large_text = MUST(String::repeated('x', 4 * MiB));
    buffer.append(log_of("small"_string));
    buffer.append(log_of(large_text));
    buffer.append(log_of(large_text));
    EXPECT_EQ(buffer.message_count(), 3u);

    // A third large message would pass the limit, so the oldest messages go until the rest fit.
    buffer.append(log_of(large_text));
    EXPECT_EQ(buffer.message_count(), 2u);
    EXPECT(buffer.size() <= WebView::ConsoleOutputBuffer::MAX_SIZE);
}

TEST_CASE(counts_text_inside_an_argument)
{
    WebView::ConsoleOutputBuffer buffer;
    JsonObject big_int;
    big_int.set("type"sv, "BigInt"sv);
    big_int.set("text"sv, MUST(String::repeated('9', 4 * MiB)));
    buffer.append({
        .timestamp = UnixDateTime::from_seconds_since_epoch(1),
        .output = WebView::ConsoleLog {
            .level = JS::ConsoleLogLevel::Log,
            .arguments = { move(big_int) },
            .type = WebView::ConsoleLogType::ConsoleAPI,
            .location = {},
            .stacktrace = {},
        },
    });
    EXPECT(buffer.size() >= 4 * MiB);
}

TEST_CASE(keeps_the_latest_message_however_large)
{
    WebView::ConsoleOutputBuffer buffer;
    buffer.append(log_of("small"_string));
    buffer.append(log_of(MUST(String::repeated('x', WebView::ConsoleOutputBuffer::MAX_SIZE + 1))));
    EXPECT_EQ(buffer.message_count(), 1u);
}
