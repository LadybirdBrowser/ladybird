/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <gtk/gtk.h>

namespace LadybirdWidgets {

template<typename T>
T* get_builder_object(GtkBuilder* builder, char const* name)
{
    auto* object = gtk_builder_get_object(builder, name);
    VERIFY(object);
    return reinterpret_cast<T*>(object);
}

}
