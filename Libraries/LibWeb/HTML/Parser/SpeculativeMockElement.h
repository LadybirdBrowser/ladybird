/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/parsing.html#speculative-mock-element
struct SpeculativeMockElement {
    // local name
    FlyString local_name;

    // attribute list
    Vector<HTMLToken::Attribute> attribute_list;

    // FIXME: namespace and children — populate when speculative tree-building is implemented.

    Optional<String> attribute(FlyString const& name) const;
};

// https://html.spec.whatwg.org/multipage/parsing.html#create-a-speculative-mock-element
SpeculativeMockElement create_a_speculative_mock_element(FlyString tag_name, Vector<HTMLToken::Attribute> attributes);

}
