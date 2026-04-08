/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <gtk/gtk.h>

namespace Ladybird {

class WebContentView;

}

struct LadybirdWebView;

GType ladybird_web_view_get_type(void);
LadybirdWebView* ladybird_web_view_new(void);
Ladybird::WebContentView* ladybird_web_view_get_impl(LadybirdWebView* self);
void ladybird_web_view_set_impl(LadybirdWebView* self, Ladybird::WebContentView* impl);
