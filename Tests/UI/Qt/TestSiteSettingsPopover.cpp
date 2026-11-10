/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/SiteSettingsPopover.h>

#include <QCheckBox>
#include <QMenu>
#include <QPushButton>
#include <QTimer>

static ByteString settings_path()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-site-popover-{}.json", Core::System::getpid())).string();
}

static void remove_settings()
{
    if (FileSystem::exists(settings_path()))
        MUST(FileSystem::remove(settings_path(), FileSystem::RecursionMode::Disallowed));
}

static Ladybird::SiteSettingsPopover::Site site_for(String host = "example.com"_string, bool is_private = false)
{
    return {
        .origin = URL::Origin { "https"_string, URL::Host { host }, {} },
        .host_for_settings = move(host),
        .is_private = is_private,
        .favicon = {},
        .content_blocking = {},
        .permissions = {},
    };
}

static QCheckBox& force_dark_switch(Ladybird::SiteSettingsPopover& popover)
{
    for (auto* widget : popover.findChildren<QCheckBox*>()) {
        if (widget->accessibleName() == "Force dark mode on this site")
            return *widget;
    }
    VERIFY_NOT_REACHED();
}

static QPushButton& reset_button(Ladybird::SiteSettingsPopover& popover)
{
    for (auto* widget : popover.findChildren<QPushButton*>()) {
        if (widget->text() == "Reset permissions")
            return *widget;
    }
    VERIFY_NOT_REACHED();
}

TEST_CASE(force_dark_switch_saves_explicit_choices_and_observes_changes)
{
    remove_settings();
    ScopeGuard cleanup { remove_settings };
    auto settings = WebView::Settings::create(settings_path());
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for());
    auto& toggle = force_dark_switch(popover);
    EXPECT(toggle.isEnabled());
    EXPECT(!toggle.isChecked());
    EXPECT(!reset_button(popover).isEnabled());
    toggle.click();
    EXPECT(settings.force_dark_for_host("example.com"sv));
    toggle.click();
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), false);
    EXPECT(reset_button(popover).isEnabled());
    settings.set_force_dark_for_host("example.com"_string, true);
    EXPECT(toggle.isChecked());
    reset_button(popover).click();
    EXPECT(!toggle.isChecked());
    EXPECT(!settings.content_settings().force_dark_per_host.contains("example.com"sv));
    EXPECT(!reset_button(popover).isEnabled());
}

TEST_CASE(private_site_controls_apply_saved_choices_without_changing_them)
{
    remove_settings();
    ScopeGuard cleanup { remove_settings };
    auto settings = WebView::Settings::create(settings_path());
    settings.set_force_dark_for_host("example.com"_string, true);
    settings.add_autoplay_site_filter("https://example.com"_string, Web::HTML::AutoplayPolicy::AllowAudioAndVideo);
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for("example.com"_string, true));
    auto& toggle = force_dark_switch(popover);
    EXPECT(!toggle.isEnabled());
    EXPECT(toggle.isChecked());
    toggle.click();
    reset_button(popover).click();
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), true);
}

TEST_CASE(blob_site_uses_the_default_without_changing_its_origin_host_override)
{
    remove_settings();
    ScopeGuard cleanup { remove_settings };
    auto settings = WebView::Settings::create(settings_path());
    settings.set_force_dark_for_host("example.com"_string, true);
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for());
    EXPECT(force_dark_switch(popover).isChecked());
    auto url = URL::Parser::basic_parse("blob:https://example.com/1234"sv).release_value();
    auto site = site_for();
    site.origin = url.origin();
    site.host_for_settings = WebView::host_for_settings(url);
    popover.set_site(move(site));
    auto& toggle = force_dark_switch(popover);
    EXPECT(!toggle.isEnabled());
    EXPECT(!toggle.isChecked());
    EXPECT(!reset_button(popover).isEnabled());
    toggle.click();
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), true);

    settings.add_autoplay_site_filter("https://example.com"_string, Web::HTML::AutoplayPolicy::AllowAudioAndVideo);
    EXPECT(reset_button(popover).isEnabled());
    reset_button(popover).click();
    EXPECT(settings.autoplay_settings().site_filters.is_empty());
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), true);

    auto content = settings.content_settings();
    content.enable_force_dark = true;
    settings.set_content_settings(move(content));
    EXPECT(toggle.isChecked());
    EXPECT(!toggle.isEnabled());
    EXPECT_EQ(settings.content_settings().force_dark_per_host.size(), 1u);
}

TEST_CASE(canceling_autoplay_does_not_reset_force_dark)
{
    remove_settings();
    ScopeGuard cleanup { remove_settings };
    auto settings = WebView::Settings::create(settings_path());
    settings.set_force_dark_for_host("example.com"_string, true);
    Core::EventLoop event_loop;
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for());
    popover.show();
    QPushButton* autoplay = nullptr;
    for (auto* button : popover.findChildren<QPushButton*>()) {
        if (button->objectName() == "LadybirdPopoverValueButton")
            autoplay = button;
    }
    VERIFY(autoplay);
    bool closed_menu = false;
    QTimer::singleShot(0, &popover, [&] {
        auto* menu = popover.findChild<QMenu*>();
        EXPECT(menu);
        if (!menu)
            return;
        closed_menu = true;
        menu->close();
    });
    autoplay->click();
    EXPECT(closed_menu);
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), true);
    EXPECT(force_dark_switch(popover).isChecked());
}
