/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SettingsPrototype.h>
#include <LibWeb/Internals/Settings.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(Settings);

Settings::Settings(JS::Realm& realm)
    : InternalsBase(realm)
{
}

Settings::~Settings() = default;

void Settings::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Settings);
}

void Settings::load_current_settings()
{
    page().client().request_current_settings();
}

void Settings::restore_default_settings()
{
    page().client().restore_default_settings();
}

void Settings::set_new_tab_page_url(String const& new_tab_page_url)
{
    if (auto parsed_new_tab_page_url = URL::Parser::basic_parse(new_tab_page_url); parsed_new_tab_page_url.has_value())
        page().client().set_new_tab_page_url(*parsed_new_tab_page_url);
}

void Settings::load_available_search_engines()
{
    page().client().request_available_search_engines();
}

void Settings::set_search_engine(Optional<String> const& search_engine)
{
    page().client().set_search_engine(search_engine);
}

}
