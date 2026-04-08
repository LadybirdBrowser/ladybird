/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGfx/Color.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SelectedFile.h>

#include <gtk/gtk.h>

namespace Ladybird {

class WebContentView;

namespace Dialogs {

void show_error(GtkWindow* parent, StringView message);
void show_alert(GtkWindow* parent, WebContentView* view, String const& message);
void show_confirm(GtkWindow* parent, WebContentView* view, String const& message);
void show_prompt(GtkWindow* parent, WebContentView* view, String const& message, String const& default_value);
void show_color_picker(GtkWindow* parent, WebContentView* view, Color current_color);
void show_file_picker(GtkWindow* parent, WebContentView* view, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple);

}

}
