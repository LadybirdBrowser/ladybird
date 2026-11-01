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
#include <LibWebView/Settings.h>
#include <UI/Qt/MacWindow.h>
#include <UI/Qt/SiteSettingsPopover.h>

#include <QCheckBox>
#include <QMenu>
#include <QPushButton>
#include <QTimer>

#if defined(AK_OS_MACOS)
// The offscreen platform has no AppKit window to animate.
void Ladybird::disable_appkit_window_animations(QWidget&)
{
}
#endif

static ByteString settings_path()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-site-popover-{}.json", Core::System::getpid())).string();
}

static void remove_settings()
{
    if (FileSystem::exists(settings_path()))
        MUST(FileSystem::remove(settings_path(), FileSystem::RecursionMode::Disallowed));
}

static Ladybird::SiteSettingsPopover::Site site_for(String force_dark_host = "example.com"_string, bool is_private = false)
{
    Ladybird::SiteSettingsPopover::Site site {};
    site.host = "example.com"_string;
    site.force_dark_host = move(force_dark_host);
    site.is_private = is_private;
    return site;
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
    settings.add_autoplay_site_filter("example.com"_string);
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for("example.com"_string, true));
    auto& toggle = force_dark_switch(popover);
    EXPECT(!toggle.isEnabled());
    EXPECT(toggle.isChecked());
    toggle.click();
    reset_button(popover).click();
    EXPECT_EQ(settings.content_settings().force_dark_per_host.get("example.com"sv), true);
}

TEST_CASE(hostless_site_uses_only_the_default)
{
    remove_settings();
    ScopeGuard cleanup { remove_settings };
    auto settings = WebView::Settings::create(settings_path());
    settings.set_force_dark_for_host("example.com"_string, true);
    Ladybird::SiteSettingsPopover popover(nullptr, settings);
    popover.set_site(site_for());
    EXPECT(force_dark_switch(popover).isChecked());
    popover.set_site(site_for({}));
    auto& toggle = force_dark_switch(popover);
    EXPECT(!toggle.isEnabled());
    EXPECT(!toggle.isChecked());
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
