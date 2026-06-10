/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

class Document;

}

namespace Web::HTML {

class CustomElementDefinition;
class CustomElementRegistry;
class HTMLElement;

}

namespace Web::Bindings {

class PlatformObject;

WEB_API void remember_custom_element_definition_prototype(HTML::CustomElementDefinition&, JS::Object&);
WEB_API void set_prototype_from_custom_element_definition_if_needed(DOM::Element&, PlatformObject&);
WEB_API JS::ThrowCompletionOr<void> upgrade_custom_element(DOM::Element&, GC::Ref<HTML::CustomElementDefinition>);
WEB_API JS::ThrowCompletionOr<GC::Ref<HTML::HTMLElement>> construct_autonomous_custom_element(DOM::Document&, FlyString const& local_name, Optional<FlyString> const& prefix, GC::Ptr<HTML::CustomElementRegistry>, GC::Ref<HTML::CustomElementDefinition>);
WEB_API void invoke_custom_element_lifecycle_callback(DOM::Element&, WebIDL::CallbackType&);
WEB_API void invoke_custom_element_callback_reaction(DOM::Element&, WebIDL::CallbackType&, DOM::CustomElementCallbackReactionArguments const&);
WEB_API void report_custom_element_upgrade_exception(HTML::CustomElementDefinition&, JS::Value exception);

}
