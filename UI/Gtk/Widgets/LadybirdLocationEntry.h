/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <gtk/gtk.h>

struct LadybirdLocationEntry;

GType ladybird_location_entry_get_type(void);
LadybirdLocationEntry* ladybird_location_entry_new(void);

void ladybird_location_entry_set_url(LadybirdLocationEntry* self, char const* url);
void ladybird_location_entry_set_text(LadybirdLocationEntry* self, char const* text);
void ladybird_location_entry_set_security_icon(LadybirdLocationEntry* self, char const* scheme);
void ladybird_location_entry_focus_and_select_all(LadybirdLocationEntry* self);
void ladybird_location_entry_set_on_navigate(LadybirdLocationEntry* self, Function<void(String)> callback);
