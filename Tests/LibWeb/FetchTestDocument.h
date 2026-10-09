/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/Platform/FontPlugin.h>

inline void install_font_plugin()
{
    static Web::Platform::FontPlugin font_plugin { false };
    static bool installed = false;
    if (installed)
        return;
    Web::Platform::FontPlugin::install(font_plugin);
    installed = true;
}

// The active document of a new top-level traversable in a new principal realm.
inline GC::Ref<Web::DOM::Document> create_test_document()
{
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& page = Web::Bindings::principal_host_defined_page(*principal_realm);
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page.set_top_level_traversable(traversable);
    return *traversable->active_document();
}
