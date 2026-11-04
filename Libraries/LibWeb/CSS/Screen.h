/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Screen.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-view-1/#screen
class Screen final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Screen, screen)

public:
    [[nodiscard]] static GC::Ref<Screen> create(HTML::Window&);
};

}
