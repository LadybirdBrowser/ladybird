/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

WEB_API bool selection_is_eligible_for_text_fragment_generation(DOM::Document const&, DOM::Range const&);
WEB_API Optional<URL::URL> generate_text_fragment_url(DOM::Document&, DOM::Range const&, URL::URL const&);

}
