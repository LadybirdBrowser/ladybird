/*
 * Copyright (c) 2025, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web {

class XMLFragmentParser final {
public:
    static WebIDL::ExceptionOr<Vector<GC::Root<DOM::Node>>> parse_xml_fragment(DOM::Element& context, StringView markup, HTML::HTMLParser::AllowDeclarativeShadowRoots = HTML::HTMLParser::AllowDeclarativeShadowRoots::No);
};

}
