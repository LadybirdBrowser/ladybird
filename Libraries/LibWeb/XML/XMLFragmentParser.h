/*
 * Copyright (c) 2025, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web {

class XMLFragmentParser final {
public:
    static WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> parse_xml_fragment(Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::DocumentFragment>> target, Utf16View markup);
};

}
