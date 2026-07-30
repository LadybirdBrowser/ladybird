/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct TextDirective {
    Optional<Utf16String> prefix;
    Utf16String start;
    Optional<Utf16String> end;
    Optional<Utf16String> suffix;
};

WEB_API Optional<String> remove_the_fragment_directive(URL::URL&);
WEB_API Optional<Utf16String> percent_decode_a_text_directive_term(Optional<StringView> term);
WEB_API Optional<TextDirective> parse_a_text_directive(StringView text_directive_value);
WEB_API Vector<TextDirective> parse_the_fragment_directive(StringView fragment_directive);
WEB_API Optional<GC::Ref<DOM::Range>> find_a_range_from_a_text_directive(TextDirective const&, DOM::Document&);

}
