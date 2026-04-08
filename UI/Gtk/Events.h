/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Cursor.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>

#include <gdk/gdk.h>

namespace Ladybird {

Web::UIEvents::MouseButton gdk_buttons_to_web(GdkModifierType state);
Web::UIEvents::MouseButton gdk_button_to_web(guint button);
Web::UIEvents::KeyModifier gdk_modifier_to_web(GdkModifierType state);
Web::UIEvents::KeyCode gdk_keyval_to_web(guint keyval);
StringView standard_cursor_to_css_name(Gfx::StandardCursor cursor);

}
