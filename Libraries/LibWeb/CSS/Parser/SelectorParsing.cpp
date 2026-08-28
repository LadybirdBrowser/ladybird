/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::CSS::Parser {

Optional<SelectorList> Parser::parse_as_selector(SelectorParsingMode mode)
{
    return parse_selector_list_in_rust(m_source, m_declared_namespaces, false, mode == SelectorParsingMode::Forgiving);
}

Optional<SelectorList> Parser::parse_as_relative_selector(SelectorParsingMode mode)
{
    return parse_selector_list_in_rust(m_source, m_declared_namespaces, true, mode == SelectorParsingMode::Forgiving);
}

Optional<Selector::PseudoElementSelector> Parser::parse_as_pseudo_element_selector()
{
    auto input_view = m_source.utf16_view();
    auto parsed = SelectorFFI::rust_selector_parse_pseudo_element({
        .ascii = input_view.has_ascii_storage() ? reinterpret_cast<u8 const*>(input_view.ascii_span().data()) : nullptr,
        .utf16 = input_view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input_view.utf16_span().data()),
        .length = input_view.length_in_code_units(),
    });
    if (!parsed.selector)
        return {};
    auto selector = Selector::create(parsed.selector);
    auto pseudo_element = pseudo_element_from_ffi(parsed.pseudo_element);
    if (!pseudo_element.has_value())
        return {};
    return Selector::PseudoElementSelector { pseudo_element.release_value(), selector->serialize() };
}

Optional<PageSelectorList> Parser::parse_as_page_selector_list()
{
    auto* list = ValueParserFFI::rust_parse_page_selector_list(ffi_utf16_view(m_source));
    if (!list)
        return {};
    ScopeGuard free_list = [&] { ValueParserFFI::rust_page_selector_list_free(list); };
    auto data = ValueParserFFI::rust_page_selector_list_data(list);
    return page_selector_list_from_rust(data);
}

}
