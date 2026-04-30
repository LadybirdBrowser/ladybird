/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>

namespace Ladybird {

class Settings {
public:
    Settings(Settings const&) = delete;
    Settings& operator=(Settings const&) = delete;

    static Settings& the()
    {
        static Settings instance;
        return instance;
    }

    struct WindowGeometry {
        int x { 0 };
        int y { 0 };
        int width { 1024 };
        int height { 768 };
        bool maximized { false };
    };

    Optional<WindowGeometry> last_window_geometry() const;
    void set_window_geometry(WindowGeometry const&);

private:
    Settings();
    ~Settings();

    GKeyFile* m_key_file { nullptr };
    char* m_path { nullptr };

    void load();
    void save() const;
};

}
