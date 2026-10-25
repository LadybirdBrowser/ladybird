/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#fragment-parser-mode
enum class FragmentParserMode : u8 {
    Safe,
    Unsafe,
    Legacy,
};

using HTMLInsertionTarget = Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::DocumentFragment>>;

HTMLInsertionTarget target_for_html_insertion(DOM::Element&);
WebIDL::ExceptionOr<Optional<HTMLInsertionTarget>> parent_for_html_insertion(DOM::Node&);

WebIDL::ExceptionOr<void> filter_and_set_html(HTMLInsertionTarget, Utf16View html, SanitizerOptions const&, FragmentParserMode);
WebIDL::ExceptionOr<void> filter_and_pre_insert_html(HTMLInsertionTarget parent, GC::Ptr<DOM::Node> child, Utf16View html, SanitizerOptions const&, FragmentParserMode);
WebIDL::ExceptionOr<void> filter_and_replace_with_html(HTMLInsertionTarget parent, DOM::Node&, Utf16View html, SanitizerOptions const&, FragmentParserMode);

WebIDL::ExceptionOr<GC::Ptr<DOM::DocumentFragment>> parse_html_for_insertion(HTMLInsertionTarget, Utf16View html, SanitizerOptions const&, FragmentParserMode);
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> fragment_parsing_algorithm_steps(HTMLInsertionTarget, Utf16View markup, SanitizerOptions const&, FragmentParserMode);

// NB: The result of the get trusted type compliant input algorithm for the legacy HTML sinks, which pass a new
//     SetHTMLUnsafeOptions dictionary. trusted_options is null if compliantOptions is that dictionary.
struct LegacySinkCompliantInput {
    Utf16String html;
    GC::Ptr<TrustedTypes::TrustedHTMLParserOptions> trusted_options;
};

enum class LegacySinkRunScripts : u8 {
    No,
    Yes,
};

WebIDL::ExceptionOr<LegacySinkCompliantInput> get_trusted_type_compliant_input_for_legacy_sink(DOM::Node&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const&, TrustedTypes::InjectionSink, LegacySinkRunScripts = LegacySinkRunScripts::No);

template<typename Options>
SanitizerOptions to_sanitizer_options(Variant<Options, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>> const& options)
{
    return options.visit(
        [](Options const& dictionary) -> SanitizerOptions { return &dictionary; },
        [](GC::Ref<TrustedTypes::TrustedHTMLParserOptions> trusted_options) -> SanitizerOptions { return trusted_options; });
}

}
