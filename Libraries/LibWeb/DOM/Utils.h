/*
 * Copyright (c) 2020, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, circl <circl.lastname@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

EventTarget* retarget(EventTarget* a, EventTarget* b);
Element* retarget_from_ua_internal_shadow_root(Element&);

}
