/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

// https://www.w3.org/TR/cssom-1/#namespacedef-css
namespace Web::CSS {

struct PropertyDefinition {
    String name;
    String syntax;
    bool inherits;
    Optional<String> initial_value;
};

WEB_API WebIDL::ExceptionOr<String> escape(JS::VM&, StringView identifier);

WEB_API bool supports(JS::VM&, StringView property, StringView value);
WEB_API WebIDL::ExceptionOr<bool> supports(JS::VM&, StringView condition_text);

WEB_API WebIDL::ExceptionOr<void> register_property(JS::VM&, PropertyDefinition definition);

}
