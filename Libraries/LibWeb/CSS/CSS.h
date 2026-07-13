/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/GeneratedCSSNumericFactoryMethods.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

// https://www.w3.org/TR/cssom-1/#namespacedef-css
namespace Web::CSS {

WEB_API WebIDL::ExceptionOr<Utf16String> escape(JS::VM&, Utf16View identifier);

WEB_API bool supports(JS::VM&, Utf16FlyString const& property, Utf16View value);
WEB_API WebIDL::ExceptionOr<bool> supports(JS::VM&, Utf16View condition_text);

WEB_API WebIDL::ExceptionOr<void> register_property(JS::VM&, Bindings::PropertyDefinition const&);

// NB: Numeric factory functions (https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory) are generated,
//     see GenerateCSSNumericFactoryMethods.cpp

}
