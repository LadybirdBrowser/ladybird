/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

enum class FocusTrigger : u8 {
    Click,
    Key,
    Script,
    Other,
};

enum class ScrollIntoView : u8 {
    No,
    Yes,
};

WEB_API Vector<GC::Root<DOM::Node>> focus_chain(GC::Ptr<DOM::Node> subject);
WEB_API void run_focus_update_steps(Vector<GC::Root<DOM::Node>> old_chain, Vector<GC::Root<DOM::Node>> new_chain, GC::Ptr<DOM::Node> new_focus_target);
WEB_API void run_focusing_steps(GC::Ptr<DOM::Node> new_focus_target, GC::Ptr<DOM::Node> fallback_target = nullptr, FocusTrigger focus_trigger = FocusTrigger::Other, ScrollIntoView = ScrollIntoView::Yes);
WEB_API void run_unfocusing_steps(GC::Ptr<DOM::Node> old_focus_target);

}
