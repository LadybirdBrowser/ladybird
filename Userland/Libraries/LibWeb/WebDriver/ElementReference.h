/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

ByteString get_or_create_a_web_element_reference(Web::DOM::Node const& element);
JsonObject web_element_reference_object(Web::DOM::Node const& element);
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(JsonObject const&);
ByteString extract_web_element_reference(JsonObject const&);
bool represents_a_web_element(JsonValue const& value);
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, Web::WebDriver::Error> get_web_element_origin(StringView origin);
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, Web::WebDriver::Error> get_known_element(StringView element_id);

bool is_element_stale(Web::DOM::Node const& element);
bool is_element_interactable(Web::HTML::BrowsingContext const&, Web::DOM::Element const&);
bool is_element_pointer_interactable(Web::HTML::BrowsingContext const&, Web::DOM::Element const&);
bool is_element_keyboard_interactable(Web::DOM::Element const&);

bool is_element_editable(Web::DOM::Element const&);
bool is_element_mutable(Web::DOM::Element const&);
bool is_element_mutable_form_control(Web::DOM::Element const&);
bool is_element_non_typeable_form_control(Web::DOM::Element const&);

ByteString get_or_create_a_shadow_root_reference(Web::DOM::ShadowRoot const& shadow_root);
JsonObject shadow_root_reference_object(Web::DOM::ShadowRoot const& shadow_root);
ErrorOr<JS::NonnullGCPtr<Web::DOM::ShadowRoot>, Web::WebDriver::Error> get_known_shadow_root(StringView shadow_id);

CSSPixelPoint in_view_center_point(DOM::Element const& element, CSSPixelRect viewport);

}
