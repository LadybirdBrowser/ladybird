/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace {

struct Globals {
    Globals();
};

Globals::Globals()
{
    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
    Web::Bindings::initialize_main_thread_vm(Web::HTML::AgentType::SimilarOriginWindow);
}

}

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    static Globals const globals;

    AK::set_debug_enabled(false);

    auto css = Utf16String::from_utf8_with_replacement_character(StringView { data, size });
    (void)Web::parse_css_stylesheet(Web::CSS::Parser::ParsingParams {}, css);
    return 0;
}
