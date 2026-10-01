/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Platform.h>
#include <LibTest/TestCase.h>
#include <dlfcn.h>

// `libgfx_rust` is compiled into more than one library: LibGfx links it, and LibWeb's Rust crate
// depends on it. A linker with a flat namespace, as Linux has, binds every call to one of the
// copies and the duplication never shows; one with two-level namespaces, as macOS has, gives each
// library its own copy of every `static` in the crate, and a counter one library increments is not
// the counter the other reads.
//
// This test makes Linux answer the macOS question. It reaches each library's own copy of the crate
// by name, checks that they really are two, and then checks that the state they share is one.
TEST_CASE(the_graphics_crate_is_compiled_twice_and_its_process_state_once)
{
#ifdef AK_OS_MACOS
    constexpr auto gfx_library_name = "liblagom-gfx.dylib"sv;
    constexpr auto web_library_name = "liblagom-web.dylib"sv;
#else
    constexpr auto gfx_library_name = "liblagom-gfx.so"sv;
    constexpr auto web_library_name = "liblagom-web.so"sv;
#endif
    auto* gfx = dlopen(ByteString { gfx_library_name }.characters(), RTLD_NOW | RTLD_LOCAL);
    auto* web = dlopen(ByteString { web_library_name }.characters(), RTLD_NOW | RTLD_LOCAL);
    // NB: dlsym() with a null handle searches the whole process, so a library that failed to open
    //     would make every check below pass vacuously.
    if (!gfx || !web) {
        FAIL(dlerror());
        return;
    }

    // Each library carries its own copy of the crate's code. If these two ever became one, the
    // test below would pass for the wrong reason, so fail loudly instead.
    auto* register_through_gfx = reinterpret_cast<void (*)()>(dlsym(gfx, "ladybird_gfx_register_rust_crate_copy"));
    auto* register_through_web = reinterpret_cast<void (*)()>(dlsym(web, "ladybird_gfx_register_rust_crate_copy"));
    auto* copies_seen_through_gfx = reinterpret_cast<size_t (*)()>(dlsym(gfx, "ladybird_gfx_process_crate_copies_seen"));
    auto* copies_seen_through_web = reinterpret_cast<size_t (*)()>(dlsym(web, "ladybird_gfx_process_crate_copies_seen"));
    if (!register_through_gfx || !register_through_web || !copies_seen_through_gfx) {
        FAIL("an entry point is missing");
        return;
    }
    EXPECT_NE(reinterpret_cast<void*>(register_through_gfx), reinterpret_cast<void*>(register_through_web));

    // The store is LibGfx's alone, so asking for it through either library is asking one store.
    EXPECT_EQ(copies_seen_through_gfx, copies_seen_through_web);

    // And both copies of the crate write into it. Were the store a `static` in the crate,
    // registering through LibWeb's copy would land in a second store and this would say one.
    register_through_gfx();
    register_through_web();
    EXPECT_EQ(copies_seen_through_gfx(), 2u);

    dlclose(web);
    dlclose(gfx);
}
