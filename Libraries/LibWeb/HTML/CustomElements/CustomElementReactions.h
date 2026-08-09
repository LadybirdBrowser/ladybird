/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

WEB_API void invoke_custom_element_reactions(Vector<GC::Root<DOM::Element>>& element_queue);
WEB_API void invoke_custom_element_reactions(Vector<GC::Weak<DOM::Element>>& element_queue);

}
