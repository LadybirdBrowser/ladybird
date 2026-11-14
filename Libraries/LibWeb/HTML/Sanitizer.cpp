/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Sanitizer.h>

namespace Web::HTML {

extern "C" void const* web_rust_sanitizer_configuration(Sanitizer const*);

SanitizerConfiguration Sanitizer::configuration() const
{
    return SanitizerConfiguration { web_rust_sanitizer_configuration(this) };
}

}
