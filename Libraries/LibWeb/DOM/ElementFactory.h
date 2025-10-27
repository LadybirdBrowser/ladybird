/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

WEB_API ErrorOr<FixedArray<FlyString>> valid_local_names_for_given_html_element_interface(StringView html_element_interface_name);
bool is_unknown_html_element(FlyString const& tag_name);

// FIXME: The spec doesn't say what the default value of synchronous_custom_elements_flag should be.
WEB_API WebIDL::ExceptionOr<GC::Ref<Element>> create_element(Document&, FlyString local_name, Optional<FlyString> const& namespace_, Optional<FlyString> prefix = {}, Optional<String> const& is = Optional<String> {}, bool synchronous_custom_elements_flag = false);

}
