/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Infra {

WebIDL::ExceptionOr<JS::Value> parse_json_string_to_javascript_value(JS::Realm&, StringView);
WebIDL::ExceptionOr<JS::Value> parse_json_bytes_to_javascript_value(JS::Realm&, ReadonlyBytes);
WebIDL::ExceptionOr<String> serialize_javascript_value_to_json_string(JS::VM&, JS::Value);
WebIDL::ExceptionOr<ByteBuffer> serialize_javascript_value_to_json_bytes(JS::VM&, JS::Value);

}
