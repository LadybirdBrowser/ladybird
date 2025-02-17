/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

GC::Ptr<Web::DOM::Node> get_node(HTML::BrowsingContext const&, StringView reference);
String get_or_create_a_node_reference(HTML::BrowsingContext const&, Web::DOM::Node const&);
bool node_reference_is_known(HTML::BrowsingContext const&, StringView reference);

String get_or_create_a_web_element_reference(HTML::BrowsingContext const&, Web::DOM::Node const& element);
JsonObject web_element_reference_object(HTML::BrowsingContext const&, Web::DOM::Node const& element);
bool represents_a_web_element(JsonValue const&);
bool represents_a_web_element(JS::Value);
ErrorOr<GC::Ref<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(Web::HTML::BrowsingContext const&, JsonObject const&);
ErrorOr<GC::Ref<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(Web::HTML::BrowsingContext const&, JS::Object const&);
String extract_web_element_reference(JsonObject const&);
ErrorOr<GC::Ref<Web::DOM::Element>, Web::WebDriver::Error> get_web_element_origin(Web::HTML::BrowsingContext const&, StringView origin);
ErrorOr<GC::Ref<Web::DOM::Element>, Web::WebDriver::Error> get_known_element(Web::HTML::BrowsingContext const&, StringView reference);

bool is_element_stale(Web::DOM::Node const& element);
bool is_element_interactable(Web::HTML::BrowsingContext const&, Web::DOM::Element const&);
bool is_element_pointer_interactable(Web::HTML::BrowsingContext const&, Web::DOM::Element const&);
bool is_element_keyboard_interactable(Web::DOM::Element const&);

bool is_element_editable(Web::DOM::Element const&);
bool is_element_mutable(Web::DOM::Element const&);
bool is_element_mutable_form_control(Web::DOM::Element const&);
bool is_element_non_typeable_form_control(Web::DOM::Element const&);

bool is_element_in_view(ReadonlySpan<GC::Ref<Web::DOM::Element>> paint_tree, Web::DOM::Element&);
bool is_element_obscured(ReadonlySpan<GC::Ref<Web::DOM::Element>> paint_tree, Web::DOM::Element&);
GC::RootVector<GC::Ref<Web::DOM::Element>> pointer_interactable_tree(Web::HTML::BrowsingContext&, Web::DOM::Element&);

String get_or_create_a_shadow_root_reference(HTML::BrowsingContext const&, Web::DOM::ShadowRoot const&);
JsonObject shadow_root_reference_object(HTML::BrowsingContext const&, Web::DOM::ShadowRoot const&);
bool represents_a_shadow_root(JsonValue const&);
bool represents_a_shadow_root(JS::Value);
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, WebDriver::Error> deserialize_shadow_root(Web::HTML::BrowsingContext const&, JsonObject const&);
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, WebDriver::Error> deserialize_shadow_root(Web::HTML::BrowsingContext const&, JS::Object const&);
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, Web::WebDriver::Error> get_known_shadow_root(HTML::BrowsingContext const&, StringView reference);
bool is_shadow_root_detached(Web::DOM::ShadowRoot const&);

String element_rendered_text(DOM::Node&);

CSSPixelPoint in_view_center_point(DOM::Element const& element, CSSPixelRect viewport);

}
