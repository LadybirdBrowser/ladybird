/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

class CustomElementRegistry;
class CustomStateSet;

}

namespace Web::Bindings {

WEB_API GC::Ref<HTML::CustomElementRegistry> construct_custom_element_registry();
WEB_API JS::ThrowCompletionOr<void> define(JS::Realm&, HTML::CustomElementRegistry&, Utf16String const&, GC::Ref<WebIDL::CallbackType>, ElementDefinitionOptions const&);
WEB_API GC::Ref<WebIDL::Promise> when_defined(JS::Realm&, HTML::CustomElementRegistry&, Utf16String const&);
WEB_API GC::Ref<JS::Set> setlike_entries(JS::Realm&, WrapperWorld const&, HTML::CustomStateSet const&);
WEB_API bool setlike_has(HTML::CustomStateSet const&, JS::Value);
WEB_API void setlike_add(HTML::CustomStateSet&, JS::Value);
WEB_API bool setlike_remove(HTML::CustomStateSet&, JS::Value);
WEB_API void setlike_clear(HTML::CustomStateSet&);
WEB_API void setlike_on_set_modified_from_js(HTML::CustomStateSet&);

}
