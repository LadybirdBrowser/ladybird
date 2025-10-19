/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

namespace Web::SanitizerAPI {

// https://wicg.github.io/sanitizer-api/#dictdef-sanitizerelementnamespace
struct SanitizerElementNamespace {
    Utf16String name;
    Optional<Utf16String> namespace_ = "http://www.w3.org/1999/xhtml"_utf16;
};

// https://wicg.github.io/sanitizer-api/#dictdef-sanitizerattributenamespace
struct SanitizerAttributeNamespace {
    Utf16String name;
    Optional<Utf16String> namespace_;
};

// https://wicg.github.io/sanitizer-api/#typedefdef-sanitizerattribute
using SanitizerAttribute = Variant<Utf16String, SanitizerAttributeNamespace>;

// https://wicg.github.io/sanitizer-api/#dictdef-sanitizerelementnamespacewithattributes
struct SanitizerElementNamespaceWithAttributes : public SanitizerElementNamespace {
    Optional<Vector<SanitizerAttribute>> attributes;
    Optional<Vector<SanitizerAttribute>> remove_attributes;
};

// https://wicg.github.io/sanitizer-api/#typedefdef-sanitizerelement
using SanitizerElement = Variant<Utf16String, SanitizerElementNamespace>;
// https://wicg.github.io/sanitizer-api/#typedefdef-sanitizerelementwithattributes
using SanitizerElementWithAttributes = Variant<Utf16String, SanitizerElementNamespaceWithAttributes>;

struct SanitizerConfig {
    Optional<Vector<SanitizerElementWithAttributes>> elements;
    Optional<Vector<SanitizerElement>> remove_elements;
    Optional<Vector<SanitizerElement>> replace_with_children_elements;

    Optional<Vector<SanitizerAttribute>> attributes;
    Optional<Vector<SanitizerAttribute>> remove_attributes;

    Optional<bool> comments;
    Optional<bool> data_attributes;
};

}
