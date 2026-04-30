/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/Settings.h>

#include <glib.h>

namespace Ladybird {

static constexpr char const* GROUP = "Window";

Settings::Settings()
{
    auto* config_dir = g_get_user_config_dir();
    m_path = g_build_filename(config_dir, "Ladybird", "ladybird-gtk.ini", nullptr);
    m_key_file = g_key_file_new();
    load();
}

Settings::~Settings()
{
    g_key_file_free(m_key_file);
    g_free(m_path);
}

void Settings::load()
{
    g_autoptr(GError) error = nullptr;
    g_key_file_load_from_file(m_key_file, m_path, G_KEY_FILE_NONE, &error);
}

void Settings::save() const
{
    g_autofree char* dir = g_path_get_dirname(m_path);
    g_mkdir_with_parents(dir, 0755);

    g_autoptr(GError) error = nullptr;
    g_key_file_save_to_file(m_key_file, m_path, &error);
}

Optional<Settings::WindowGeometry> Settings::last_window_geometry() const
{
    if (!g_key_file_has_group(m_key_file, GROUP))
        return {};

    WindowGeometry g;
    g.x = static_cast<int>(g_key_file_get_integer(m_key_file, GROUP, "x", nullptr));
    g.y = static_cast<int>(g_key_file_get_integer(m_key_file, GROUP, "y", nullptr));
    g.width = static_cast<int>(g_key_file_get_integer(m_key_file, GROUP, "width", nullptr));
    g.height = static_cast<int>(g_key_file_get_integer(m_key_file, GROUP, "height", nullptr));
    g.maximized = g_key_file_get_boolean(m_key_file, GROUP, "maximized", nullptr);

    if (g.width <= 0)
        g.width = 1024;
    if (g.height <= 0)
        g.height = 768;

    return g;
}

void Settings::set_window_geometry(WindowGeometry const& g)
{
    g_key_file_set_integer(m_key_file, GROUP, "x", g.x);
    g_key_file_set_integer(m_key_file, GROUP, "y", g.y);
    g_key_file_set_integer(m_key_file, GROUP, "width", g.width);
    g_key_file_set_integer(m_key_file, GROUP, "height", g.height);
    g_key_file_set_boolean(m_key_file, GROUP, "maximized", g.maximized);
    save();
}

}
