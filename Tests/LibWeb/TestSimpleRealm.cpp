/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Window.h>

// NOTE: These tests use the persistent main-thread VM, so they must not share a
// process with tests that create their own JS::VM.

TEST_CASE(simple_javascript_realm_caches_global_window_wrapper)
{
    auto realm = Web::Bindings::create_a_simple_javascript_realm();

    auto* global = as_if<Web::Bindings::PlatformObject>(&realm->global_object());
    EXPECT(global);
    auto* window = Web::Bindings::impl_from<Web::HTML::Window>(global);
    EXPECT(window);

    // Wrapping the global's impl in its own realm must return the global itself,
    // not mint a second Window identity.
    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(*realm);
    auto wrapper = Web::Bindings::wrap(wrapper_world, *realm, GC::Ref<Web::Bindings::Wrappable> { static_cast<Web::Bindings::Wrappable&>(*window) });
    EXPECT(wrapper.ptr() == global);
    EXPECT(wrapper_world.wrapper_for(*window, *realm).ptr() == global);
}
