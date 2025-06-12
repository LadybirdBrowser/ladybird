/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

// https://drafts.csswg.org/web-animations-1/#dom-keyframeeffect-pseudo-element-parsing
WebIDL::ExceptionOr<Optional<CSS::Selector::PseudoElementSelector>> pseudo_element_parsing(JS::Realm&, Optional<String> const&);

}
