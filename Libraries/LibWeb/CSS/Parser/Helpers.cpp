/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/Window.h>

namespace Web {

GC::Ref<JS::Realm> internal_css_realm()
{
    static GC::Root<JS::Realm> realm;
    static GC::Root<HTML::Window> window;
    static OwnPtr<JS::ExecutionContext> execution_context;
    if (!realm) {
        execution_context = Bindings::create_a_new_javascript_realm(
            Bindings::main_thread_vm(),
            [&](JS::Realm& realm) -> JS::Object* {
                window = HTML::Window::create(realm);
                return window;
            },
            [&](JS::Realm&) -> JS::Object* {
                return window;
            });

        realm = *execution_context->realm;
        auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
        auto host_defined = make<Bindings::HostDefined>(intrinsics);
        realm->set_host_defined(move(host_defined));
    }
    return *realm;
}

CSS::CSSStyleSheet* parse_css_stylesheet(CSS::Parser::ParsingParams const& context, StringView css, Optional<URL::URL> location)
{
    if (css.is_empty()) {
        auto rule_list = CSS::CSSRuleList::create_empty(*context.realm);
        auto media_list = CSS::MediaList::create(*context.realm, {});
        auto style_sheet = CSS::CSSStyleSheet::create(*context.realm, rule_list, media_list, location);
        style_sheet->set_source_text({});
        return style_sheet;
    }
    auto* style_sheet = CSS::Parser::Parser::create(context, css).parse_as_css_stylesheet(location);
    // FIXME: Avoid this copy
    style_sheet->set_source_text(MUST(String::from_utf8(css)));
    return style_sheet;
}

CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_style_attribute(CSS::Parser::ParsingParams const& context, StringView css)
{
    if (css.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, css).parse_as_style_attribute();
}

RefPtr<CSS::CSSStyleValue> parse_css_value(CSS::Parser::ParsingParams const& context, StringView string, CSS::PropertyID property_id)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(context, string).parse_as_css_value(property_id);
}

CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const& context, StringView css_text)
{
    return CSS::Parser::Parser::create(context, css_text).parse_as_css_rule();
}

Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_selector();
}

Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    auto parser = CSS::Parser::Parser::create(context, selector_text);

    auto maybe_selectors = parser.parse_as_relative_selector(CSS::Parser::Parser::SelectorParsingMode::Standard);
    if (!maybe_selectors.has_value())
        return {};

    return adapt_nested_relative_selector_list(*maybe_selectors);
}

Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_pseudo_element_selector();
}

RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query();
}

Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query_list();
}

RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const& context, StringView string)
{
    if (string.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, string).parse_as_supports();
}

}
