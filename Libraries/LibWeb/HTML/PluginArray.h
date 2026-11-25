/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PluginArray.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#pluginarray
class PluginArray final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(PluginArray, plugin_array)

public:
    [[nodiscard]] static GC::Ref<PluginArray> create(Window&);
};

}
