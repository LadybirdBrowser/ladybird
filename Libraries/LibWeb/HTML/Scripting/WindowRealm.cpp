/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/WindowRealm.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web::HTML {

NonnullOwnPtr<JS::ExecutionContext> create_window_realm(GC::Ptr<Window>& window, BrowsingContext& browsing_context)
{
    return Bindings::create_a_new_javascript_realm(
        Bindings::main_thread_vm(),
        [&](JS::Realm& realm) -> JS::Object* {
            // For the global object, create a new Window object.
            window = Window::create();
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *window }).ptr();
        },
        [&](JS::Realm&) -> JS::Object* {
            // For the global this binding, use browsingContext's WindowProxy object.
            return browsing_context.window_proxy();
        });
}

}
