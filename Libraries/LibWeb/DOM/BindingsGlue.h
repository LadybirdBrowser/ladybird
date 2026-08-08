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
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::DOM {

class Node;
class NodeFilter;
class NodeIterator;
class TreeWalker;
class Event;
class EventTarget;
class Document;
class ShadowRoot;

}

namespace Web::Bindings {

WEB_API JS::ThrowCompletionOr<DOM::NodeFilter::Result> accept_node(JS::Realm&, DOM::NodeFilter&, DOM::Node&);
WEB_API JS::Value filter(DOM::NodeIterator&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm&, DOM::NodeIterator&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm&, DOM::NodeIterator&);
WEB_API JS::Value filter(DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> parent_node(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> first_child(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> last_child(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_sibling(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_sibling(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm&, DOM::TreeWalker&);

WEB_API void add_event_listener(DOM::EventTarget&, Utf16String const&, DOM::IDLEventListener*, Variant<AddEventListenerOptions, bool> const&);
WEB_API void remove_event_listener(DOM::EventTarget&, Utf16String const&, DOM::IDLEventListener*, Variant<EventListenerOptions, bool> const&);
WEB_API GC::Ref<JS::Environment> new_event_handler_object_environment(JS::Realm&, GC::Ref<DOM::EventTarget>, GC::Ref<JS::Environment>);
WEB_API JS::Realm* callback_realm(WebIDL::CallbackType*);
WEB_API HTML::Window* window_from_callback(WebIDL::CallbackType&);
WEB_API void report_exception_for_callback(WebIDL::CallbackType&, JS::Value);
WEB_API JS::Completion invoke_event_listener(WebIDL::CallbackType&, DOM::Event&);
WEB_API JS::Completion invoke_event_handler(WebIDL::CallbackType&, DOM::Event&, bool);
WEB_API DOM::Event* event_from_value(JS::Value);
WEB_API DOM::Event* event_from_callback_argument(JS::VM&);
WEB_API JS::Value event(JS::Realm&, GC::Ref<DOM::Event>);
WEB_API GC::Ptr<PlatformObject> current_target_wrapper(JS::Realm&, DOM::Event const&);
WEB_API JS::Value current_target_value(JS::Realm&, DOM::Event const&);

WEB_API JS::Value document(JS::Realm&, GC::Ref<DOM::Document>);
WEB_API GC::Ref<CSS::StyleSheetList> style_sheets(DOM::Document&);
WEB_API JS::Value adopted_style_sheets(DOM::Document&);
WEB_API WebIDL::ExceptionOr<void> set_adopted_style_sheets(DOM::Document&, JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Root<DOM::Document>> parse_html_unsafe(JS::Realm&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<void> write(DOM::Document&, Vector<TrustedTypes::TrustedHTMLOrString> const&);
WEB_API WebIDL::ExceptionOr<void> writeln(DOM::Document&, Vector<TrustedTypes::TrustedHTMLOrString> const&);
WEB_API GC::Ptr<ViewTransition::ViewTransition> start_view_transition(DOM::Document&, GC::Ptr<WebIDL::CallbackType> update_callback);
WEB_API JS::Value document_named_item_value(WrapperWorld&, JS::Realm&, DOM::Document const&, Utf16FlyString const& name);

WEB_API WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> inner_html(DOM::ShadowRoot&);
WEB_API WebIDL::ExceptionOr<void> set_inner_html(DOM::ShadowRoot&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<void> set_html_unsafe(DOM::ShadowRoot&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html);
WEB_API GC::Ref<CSS::StyleSheetList> style_sheets(DOM::ShadowRoot&);
WEB_API JS::Value adopted_style_sheets(DOM::ShadowRoot&);
WEB_API WebIDL::ExceptionOr<void> set_adopted_style_sheets(DOM::ShadowRoot&, JS::Value);

}
