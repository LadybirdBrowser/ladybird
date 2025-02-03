/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

JsonObject window_proxy_reference_object(HTML::WindowProxy const&);

bool represents_a_web_frame(JS::Value);
ErrorOr<GC::Ref<HTML::WindowProxy>, WebDriver::Error> deserialize_web_frame(JS::Object const&);

bool represents_a_web_window(JS::Value);
ErrorOr<GC::Ref<HTML::WindowProxy>, WebDriver::Error> deserialize_web_window(JS::Object const&);

ErrorOr<void, WebDriver::Error> ensure_browsing_context_is_open(GC::Ptr<HTML::BrowsingContext>);

}
