/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibURL/Pattern/Options.h>
#include <LibURL/Pattern/Part.h>

namespace URL::Pattern {

String escape_a_pattern_string(String const&);
String escape_a_regexp_string(String const&);
String generate_a_segment_wildcard_regexp(Options const&);
String generate_a_pattern_string(ReadonlySpan<Part>, Options const&);

}
