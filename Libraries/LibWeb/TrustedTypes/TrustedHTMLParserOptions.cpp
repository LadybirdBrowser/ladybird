/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedHTMLParserOptions);

GC::Ref<TrustedHTMLParserOptions> TrustedHTMLParserOptions::create(Optional<HTML::SanitizerConfiguration> sanitizer_configuration, bool run_scripts)
{
    return GC::Heap::the().allocate<TrustedHTMLParserOptions>(move(sanitizer_configuration), run_scripts);
}

TrustedHTMLParserOptions::TrustedHTMLParserOptions(Optional<HTML::SanitizerConfiguration> sanitizer_configuration, bool run_scripts)
    : m_sanitizer_configuration(move(sanitizer_configuration))
    , m_run_scripts(run_scripts)
{
}

TrustedHTMLParserOptions::~TrustedHTMLParserOptions() = default;

}
