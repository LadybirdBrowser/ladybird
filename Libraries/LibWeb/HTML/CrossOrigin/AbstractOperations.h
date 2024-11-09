/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>

namespace Web::HTML {

Vector<CrossOriginProperty> cross_origin_properties(Variant<HTML::Location const*, HTML::Window const*> const&);
bool is_cross_origin_accessible_window_property_name(JS::PropertyKey const&);
JS::ThrowCompletionOr<JS::PropertyDescriptor> cross_origin_property_fallback(JS::VM&, JS::PropertyKey const&);
bool is_platform_object_same_origin(JS::Object const&);
Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(Variant<HTML::Location*, HTML::Window*> const&, JS::PropertyKey const&);
JS::ThrowCompletionOr<JS::Value> cross_origin_get(JS::VM&, JS::Object const&, JS::PropertyKey const&, JS::Value receiver);
JS::ThrowCompletionOr<bool> cross_origin_set(JS::VM&, JS::Object&, JS::PropertyKey const&, JS::Value, JS::Value receiver);
JS::MarkedVector<JS::Value> cross_origin_own_property_keys(Variant<HTML::Location const*, HTML::Window const*> const&);

}
