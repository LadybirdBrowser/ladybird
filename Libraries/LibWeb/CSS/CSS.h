/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/GeneratedCSSNumericFactoryMethods.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

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

// NB: Numeric factory functions (https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory) are generated,
//     see GenerateCSSNumericFactoryMethods.cpp

}
