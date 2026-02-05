/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/AttributeNames.h>

namespace Web::HTML {
namespace AttributeNames {

#define __ENUMERATE_HTML_ATTRIBUTE(name, attribute) \
    FlyString name = attribute##_fly_string;
ENUMERATE_HTML_ATTRIBUTES
#undef __ENUMERATE_HTML_ATTRIBUTE

}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#boolean-attribute
bool is_boolean_attribute(FlyString const& attribute)
{
    // NOTE: For web compatibility, this matches the list of attributes which Chromium considers to be booleans,
    //       excluding attributes that are only used by Chromium itself:
    //       https://source.chromium.org/chromium/chromium/src/+/460b7c003cf89fc9493e721701906f19e5f6a387:chrome/test/chromedriver/element_commands.cc;l=48-94
    return attribute.equals_ignoring_ascii_case(AttributeNames::allowfullscreen)
        || attribute.equals_ignoring_ascii_case(AttributeNames::async)
        || attribute.equals_ignoring_ascii_case(AttributeNames::autofocus)
        || attribute.equals_ignoring_ascii_case(AttributeNames::autoplay)
        || attribute.equals_ignoring_ascii_case(AttributeNames::checked)
        || attribute.equals_ignoring_ascii_case(AttributeNames::compact)
        || attribute.equals_ignoring_ascii_case(AttributeNames::controls)
        || attribute.equals_ignoring_ascii_case(AttributeNames::declare)
        || attribute.equals_ignoring_ascii_case(AttributeNames::default_)
        || attribute.equals_ignoring_ascii_case(AttributeNames::defaultchecked)
        || attribute.equals_ignoring_ascii_case(AttributeNames::defaultselected)
        || attribute.equals_ignoring_ascii_case(AttributeNames::defer)
        || attribute.equals_ignoring_ascii_case(AttributeNames::disabled)
        || attribute.equals_ignoring_ascii_case(AttributeNames::ended)
        || attribute.equals_ignoring_ascii_case(AttributeNames::formnovalidate)
        || attribute.equals_ignoring_ascii_case(AttributeNames::hidden)
        || attribute.equals_ignoring_ascii_case(AttributeNames::indeterminate)
        || attribute.equals_ignoring_ascii_case(AttributeNames::iscontenteditable)
        || attribute.equals_ignoring_ascii_case(AttributeNames::ismap)
        || attribute.equals_ignoring_ascii_case(AttributeNames::itemscope)
        || attribute.equals_ignoring_ascii_case(AttributeNames::loop)
        || attribute.equals_ignoring_ascii_case(AttributeNames::multiple)
        || attribute.equals_ignoring_ascii_case(AttributeNames::muted)
        || attribute.equals_ignoring_ascii_case(AttributeNames::nohref)
        || attribute.equals_ignoring_ascii_case(AttributeNames::nomodule)
        || attribute.equals_ignoring_ascii_case(AttributeNames::noresize)
        || attribute.equals_ignoring_ascii_case(AttributeNames::noshade)
        || attribute.equals_ignoring_ascii_case(AttributeNames::novalidate)
        || attribute.equals_ignoring_ascii_case(AttributeNames::nowrap)
        || attribute.equals_ignoring_ascii_case(AttributeNames::open)
        || attribute.equals_ignoring_ascii_case(AttributeNames::paused)
        || attribute.equals_ignoring_ascii_case(AttributeNames::playsinline)
        || attribute.equals_ignoring_ascii_case(AttributeNames::readonly)
        || attribute.equals_ignoring_ascii_case(AttributeNames::required)
        || attribute.equals_ignoring_ascii_case(AttributeNames::reversed)
        || attribute.equals_ignoring_ascii_case(AttributeNames::seeking)
        || attribute.equals_ignoring_ascii_case(AttributeNames::selected)
        || attribute.equals_ignoring_ascii_case(AttributeNames::switch_)
        || attribute.equals_ignoring_ascii_case(AttributeNames::truespeed)
        || attribute.equals_ignoring_ascii_case(AttributeNames::willvalidate);
}

}
