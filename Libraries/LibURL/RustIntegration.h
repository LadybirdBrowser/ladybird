/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>

namespace URL::RustIntegration {

Optional<URL> parse_basic_url(StringView input, Optional<URL const&> base_url = {}, URL* url = nullptr, Optional<Parser::State> state_override = {}, Optional<StringView> encoding = {});

}
