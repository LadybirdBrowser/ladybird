/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/SystemAppearance.h>

// Qt's signals macro clashes with a member of the same name in GLib's D-Bus headers.
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")

using Ladybird::SystemAppearance;

static constexpr auto APPEARANCE = "org.freedesktop.appearance"sv;
static constexpr auto GNOME_INTERFACE = "org.gnome.desktop.interface"sv;
static constexpr auto KDE_GLOBALS = "org.kde.kdeglobals.KDE"sv;

static void apply(StringView namespace_, StringView key, GVariant* value)
{
    g_variant_ref_sink(value);
    SystemAppearance::the().apply_portal_setting(namespace_, key, value);
    g_variant_unref(value);
}

static void reset_motion_settings()
{
    // A value of the wrong type leaves the portal's own setting unset, as a portal without it does.
    apply(APPEARANCE, "reduced-motion"sv, g_variant_new_string(""));
    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(true));
    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_double(1));
}

TEST_CASE(gnome_animations_switched_off_ask_for_less_motion)
{
    reset_motion_settings();
    size_t changes = 0;
    auto connection = QObject::connect(&SystemAppearance::the(), &SystemAppearance::reduced_motion_changed, [&] { ++changes; });

    EXPECT(!SystemAppearance::prefers_reduced_motion());
    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(false));
    EXPECT(SystemAppearance::prefers_reduced_motion());
    EXPECT_EQ(changes, 1u);

    // The same value again is no change.
    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(false));
    EXPECT_EQ(changes, 1u);

    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(true));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
    EXPECT_EQ(changes, 2u);

    QObject::disconnect(connection);
}

TEST_CASE(kde_instant_animations_ask_for_less_motion)
{
    reset_motion_settings();

    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_double(0));
    EXPECT(SystemAppearance::prefers_reduced_motion());
    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_double(0.5));
    EXPECT(!SystemAppearance::prefers_reduced_motion());

    // KDE's portal can report the factor as the text it read from kdeglobals.
    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_string("0"));
    EXPECT(SystemAppearance::prefers_reduced_motion());
    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_string("2"));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
}

TEST_CASE(either_desktop_asking_for_less_motion_is_enough)
{
    reset_motion_settings();

    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_double(0));
    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(true));
    EXPECT(SystemAppearance::prefers_reduced_motion());
}

TEST_CASE(the_portals_reduced_motion_setting_asks_for_less_motion)
{
    reset_motion_settings();
    size_t changes = 0;
    auto connection = QObject::connect(&SystemAppearance::the(), &SystemAppearance::reduced_motion_changed, [&] { ++changes; });

    apply(APPEARANCE, "reduced-motion"sv, g_variant_new_uint32(1));
    EXPECT(SystemAppearance::prefers_reduced_motion());
    EXPECT_EQ(changes, 1u);
    apply(APPEARANCE, "reduced-motion"sv, g_variant_new_uint32(0));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
    EXPECT_EQ(changes, 2u);

    // Values the portal doesn't define mean no preference.
    apply(APPEARANCE, "reduced-motion"sv, g_variant_new_uint32(2));
    EXPECT(!SystemAppearance::prefers_reduced_motion());

    QObject::disconnect(connection);
}

TEST_CASE(the_portals_reduced_motion_setting_comes_before_the_desktops_own)
{
    reset_motion_settings();

    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(false));
    apply(KDE_GLOBALS, "AnimationDurationFactor"sv, g_variant_new_double(0));
    apply(APPEARANCE, "reduced-motion"sv, g_variant_new_uint32(0));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
}

TEST_CASE(settings_of_the_same_name_in_other_namespaces_are_ignored)
{
    reset_motion_settings();

    apply("org.freedesktop.appearance"sv, "enable-animations"sv, g_variant_new_boolean(false));
    apply("org.kde.kdeglobals.General"sv, "AnimationDurationFactor"sv, g_variant_new_double(0));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
}

TEST_CASE(a_value_of_the_wrong_type_follows_no_preference)
{
    reset_motion_settings();

    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_boolean(false));
    EXPECT(SystemAppearance::prefers_reduced_motion());
    apply(GNOME_INTERFACE, "enable-animations"sv, g_variant_new_uint32(0));
    EXPECT(!SystemAppearance::prefers_reduced_motion());
}
