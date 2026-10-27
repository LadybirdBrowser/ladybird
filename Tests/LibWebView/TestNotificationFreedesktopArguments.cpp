/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/NotificationFreedesktopArguments.h>

using namespace WebView;

static PlatformNotification notification(Utf16String title, Utf16String body)
{
    PlatformNotification notification;
    notification.id = 7;
    notification.data.title = move(title);
    notification.data.body = move(body);
    notification.attribution = "example.com"_string;
    return notification;
}

static Optional<FreedesktopHint> hint(FreedesktopNotifyArguments const& arguments, StringView name)
{
    for (auto const& hint : arguments.hints) {
        if (hint.name == name)
            return hint;
    }
    return {};
}

TEST_CASE(summary_is_the_title_and_body_opens_with_the_attribution)
{
    auto arguments = build_notify_arguments(notification("New mail"_utf16, "From Alice"_utf16), { .body = true }, 0);
    EXPECT_EQ(arguments.app_name, "Ladybird"_string);
    EXPECT_EQ(arguments.summary, "New mail"_string);
    EXPECT_EQ(arguments.body, "example.com\n\nFrom Alice"_string);
    EXPECT_EQ(arguments.replaces_id, 0u);
}

TEST_CASE(replaced_daemon_id_passes_through)
{
    auto arguments = build_notify_arguments(notification("t"_utf16, "b"_utf16), { .body = true }, 42);
    EXPECT_EQ(arguments.replaces_id, 42u);
}

TEST_CASE(markup_is_escaped_only_when_the_daemon_renders_it)
{
    auto plain = build_notify_arguments(notification("<b>&</b>"_utf16, "a < b & c"_utf16), { .body = true }, 0);
    EXPECT_EQ(plain.summary, "<b>&</b>"_string);
    EXPECT_EQ(plain.body, "example.com\n\na < b & c"_string);

    auto markup = build_notify_arguments(notification("<b>&</b>"_utf16, "a < b & c"_utf16), { .body = true, .body_markup = true }, 0);
    EXPECT_EQ(markup.summary, "<b>&</b>"_string);
    EXPECT_EQ(markup.body, "example.com\n\na &lt; b &amp; c"_string);
}

TEST_CASE(daemon_without_body_support_gets_no_body)
{
    auto arguments = build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0);
    EXPECT_EQ(arguments.body, ""_string);
}

TEST_CASE(expire_timeout_follows_persistence_and_require_interaction)
{
    EXPECT_EQ(build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0).expire_timeout, 25000);
    EXPECT_EQ(build_notify_arguments(notification("t"_utf16, "b"_utf16), { .persistence = true }, 0).expire_timeout, -1);

    auto sticky = notification("t"_utf16, "b"_utf16);
    sticky.data.require_interaction = true;
    EXPECT_EQ(build_notify_arguments(sticky, { .persistence = true }, 0).expire_timeout, 0);
}

TEST_CASE(urgency_is_critical_only_when_interaction_is_required)
{
    auto normal = hint(build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0), "urgency"sv);
    EXPECT(normal.has_value());
    EXPECT_EQ(normal->value.get<u8>(), 1u);

    auto sticky = notification("t"_utf16, "b"_utf16);
    sticky.data.require_interaction = true;
    auto critical = hint(build_notify_arguments(sticky, {}, 0), "urgency"sv);
    EXPECT_EQ(critical->value.get<u8>(), 2u);
}

TEST_CASE(silent_suppresses_the_sound)
{
    auto loud = hint(build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0), "suppress-sound"sv);
    EXPECT(!loud.has_value());

    auto quiet_notification = notification("t"_utf16, "b"_utf16);
    quiet_notification.data.silent = true;
    auto quiet = hint(build_notify_arguments(quiet_notification, {}, 0), "suppress-sound"sv);
    EXPECT(quiet.has_value());
    EXPECT_EQ(quiet->value.get<bool>(), true);
}

TEST_CASE(desktop_entry_names_the_application)
{
    auto entry = hint(build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0), "desktop-entry"sv);
    EXPECT(entry.has_value());
    EXPECT_EQ(entry->value.get<String>(), "org.ladybird.Ladybird"_string);
}

TEST_CASE(default_action_only_when_the_daemon_takes_actions)
{
    EXPECT(build_notify_arguments(notification("t"_utf16, "b"_utf16), {}, 0).actions.is_empty());
    auto with_actions = build_notify_arguments(notification("t"_utf16, "b"_utf16), { .actions = true }, 0);
    EXPECT_EQ(with_actions.actions.size(), 2u);
    EXPECT_EQ(with_actions.actions[0], "default"_string);
}

TEST_CASE(icon_becomes_image_data)
{
    auto with_icon = notification("t"_utf16, "b"_utf16);
    with_icon.icon = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 1 }));
    auto image = hint(build_notify_arguments(with_icon, {}, 0), "image-data"sv);
    EXPECT(image.has_value());
    auto const& data = image->value.get<FreedesktopImageData>();
    EXPECT_EQ(data.width, 2);
    EXPECT_EQ(data.height, 1);
    EXPECT_EQ(data.rowstride, 8);
    EXPECT_EQ(data.has_alpha, true);
    EXPECT_EQ(data.bits_per_sample, 8);
    EXPECT_EQ(data.channels, 4);
    EXPECT_EQ(data.pixels.size(), 8u);
}
