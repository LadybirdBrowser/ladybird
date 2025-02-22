/*
 * Copyright (c) 2019-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/Bindings/CSSStyleSheetPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleSheet);

GC::Ref<CSSStyleSheet> CSSStyleSheet::create(JS::Realm& realm, CSSRuleList& rules, MediaList& media, Optional<URL::URL> location)
{
    return realm.create<CSSStyleSheet>(realm, rules, media, move(location));
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-cssstylesheet
WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> CSSStyleSheet::construct_impl(JS::Realm& realm, Optional<CSSStyleSheetInit> const& options)
{
    // 1. Construct a new CSSStyleSheet object sheet.
    auto sheet = create(realm, CSSRuleList::create_empty(realm), CSS::MediaList::create(realm, {}), {});

    // 2. Set sheet’s location to the base URL of the associated Document for the current principal global object.
    auto associated_document = as<HTML::Window>(HTML::current_principal_global_object()).document();
    sheet->set_location(associated_document->base_url().to_string());

    // 3. Set sheet’s stylesheet base URL to the baseURL attribute value from options.
    if (options.has_value() && options->base_url.has_value()) {
        Optional<URL::URL> sheet_location_url;
        if (sheet->location().has_value())
            sheet_location_url = URL::Parser::basic_parse(sheet->location().release_value());

        // AD-HOC: This isn't explicitly mentioned in the specification, but multiple modern browsers do this.
        Optional<URL::URL> url = sheet->location().has_value() ? sheet_location_url->complete_url(options->base_url.value()) : URL::Parser::basic_parse(options->base_url.value());
        if (!url.has_value())
            return WebIDL::NotAllowedError::create(realm, "Constructed style sheets must have a valid base URL"_string);

        sheet->set_base_url(url);
    }

    // 4. Set sheet’s parent CSS style sheet to null.
    sheet->set_parent_css_style_sheet(nullptr);

    // 5. Set sheet’s owner node to null.
    sheet->set_owner_node(nullptr);

    // 6. Set sheet’s owner CSS rule to null.
    sheet->set_owner_css_rule(nullptr);

    // 7. Set sheet’s title to the empty string.
    sheet->set_title(String {});

    // 8. Unset sheet’s alternate flag.
    sheet->set_alternate(false);

    // 9. Set sheet’s origin-clean flag.
    sheet->set_origin_clean(true);

    // 10. Set sheet’s constructed flag.
    sheet->set_constructed(true);

    // 11. Set sheet’s Constructor document to the associated Document for the current global object.
    sheet->set_constructor_document(associated_document);

    // 12. If the media attribute of options is a string, create a MediaList object from the string and assign it as sheet’s media.
    //     Otherwise, serialize a media query list from the attribute and then create a MediaList object from the resulting string and set it as sheet’s media.
    if (options.has_value()) {
        if (options->media.has<String>()) {
            sheet->set_media(options->media.get<String>());
        } else {
            sheet->m_media = *options->media.get<GC::Root<MediaList>>();
        }
    }

    // 13. If the disabled attribute of options is true, set sheet’s disabled flag.
    if (options.has_value() && options->disabled)
        sheet->set_disabled(true);

    // 14. Return sheet
    return sheet;
}

CSSStyleSheet::CSSStyleSheet(JS::Realm& realm, CSSRuleList& rules, MediaList& media, Optional<URL::URL> location)
    : StyleSheet(realm, media)
    , m_rules(&rules)
{
    if (location.has_value())
        set_location(location->to_string());

    for (auto& rule : *m_rules)
        rule->set_parent_style_sheet(this);

    recalculate_rule_caches();

    m_rules->on_change = [this]() {
        recalculate_rule_caches();
    };
}

void CSSStyleSheet::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleSheet);
}

void CSSStyleSheet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
    visitor.visit(m_owner_css_rule);
    visitor.visit(m_default_namespace_rule);
    visitor.visit(m_constructor_document);
    visitor.visit(m_namespace_rules);
    visitor.visit(m_import_rules);
    visitor.visit(m_owning_documents_or_shadow_roots);
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-insertrule
WebIDL::ExceptionOr<unsigned> CSSStyleSheet::insert_rule(StringView rule, unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create(realm(), "Can't call insert_rule() on non-modifiable stylesheets."_string);

    // 3. Let parsed rule be the return value of invoking parse a rule with rule.
    auto context = !m_owning_documents_or_shadow_roots.is_empty() ? Parser::ParsingParams { (*m_owning_documents_or_shadow_roots.begin())->document() } : Parser::ParsingParams { realm() };
    auto parsed_rule = parse_css_rule(context, rule);

    // 4. If parsed rule is a syntax error, return parsed rule.
    if (!parsed_rule)
        return WebIDL::SyntaxError::create(realm(), "Unable to parse CSS rule."_string);

    // 5. If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException.
    if (constructed() && parsed_rule->type() == CSSRule::Type::Import)
        return WebIDL::SyntaxError::create(realm(), "Can't insert @import rules into a constructed stylesheet."_string);

    // 6. Return the result of invoking insert a CSS rule rule in the CSS rules at index.
    auto result = m_rules->insert_a_css_rule(parsed_rule, index);

    if (!result.is_exception()) {
        // NOTE: The spec doesn't say where to set the parent style sheet, so we'll do it here.
        parsed_rule->set_parent_style_sheet(this);

        invalidate_owners(DOM::StyleInvalidationReason::StyleSheetInsertRule);
    }

    return result;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-deleterule
WebIDL::ExceptionOr<void> CSSStyleSheet::delete_rule(unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // 2. If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create(realm(), "Can't call delete_rule() on non-modifiable stylesheets."_string);

    // 3. Remove a CSS rule in the CSS rules at index.
    auto result = m_rules->remove_a_css_rule(index);
    if (!result.is_exception()) {
        invalidate_owners(DOM::StyleInvalidationReason::StyleSheetDeleteRule);
    }
    return result;
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replace
GC::Ref<WebIDL::Promise> CSSStyleSheet::replace(String text)
{
    auto& realm = this->realm();

    // 1. Let promise be a promise
    auto promise = WebIDL::create_promise(realm);

    // 2. If the constructed flag is not set, or the disallow modification flag is set, reject promise with a NotAllowedError DOMException and return promise.
    if (!constructed()) {
        WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Can't call replace() on non-constructed stylesheets"_string));
        return promise;
    }

    if (disallow_modification()) {
        WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Can't call replace() on non-modifiable stylesheets"_string));
        return promise;
    }

    // 3. Set the disallow modification flag.
    set_disallow_modification(true);

    // 4. In parallel, do these steps:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, this, text = move(text), promise = GC::Root(promise)] {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let rules be the result of running parse a stylesheet’s contents from text.
        auto context = !m_owning_documents_or_shadow_roots.is_empty() ? Parser::ParsingParams { (*m_owning_documents_or_shadow_roots.begin())->document() } : CSS::Parser::ParsingParams { realm };
        auto* parsed_stylesheet = parse_css_stylesheet(context, text);
        auto& rules = parsed_stylesheet->rules();

        // 2. If rules contains one or more @import rules, remove those rules from rules.
        GC::RootVector<GC::Ref<CSSRule>> rules_without_import(realm.heap());
        for (auto rule : rules) {
            if (rule->type() != CSSRule::Type::Import)
                rules_without_import.append(rule);
        }

        // 3. Set sheet’s CSS rules to rules.
        m_rules->set_rules({}, rules_without_import);

        // 4. Unset sheet’s disallow modification flag.
        set_disallow_modification(false);

        // 5. Resolve promise with sheet.
        WebIDL::resolve_promise(realm, *promise, this);
    }));

    return promise;
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replacesync
WebIDL::ExceptionOr<void> CSSStyleSheet::replace_sync(StringView text)
{
    // 1. If the constructed flag is not set, or the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (!constructed())
        return WebIDL::NotAllowedError::create(realm(), "Can't call replaceSync() on non-constructed stylesheets"_string);
    if (disallow_modification())
        return WebIDL::NotAllowedError::create(realm(), "Can't call replaceSync() on non-modifiable stylesheets"_string);

    // 2. Let rules be the result of running parse a stylesheet’s contents from text.
    auto context = !m_owning_documents_or_shadow_roots.is_empty() ? Parser::ParsingParams { (*m_owning_documents_or_shadow_roots.begin())->document() } : CSS::Parser::ParsingParams { realm() };
    auto* parsed_stylesheet = parse_css_stylesheet(context, text);
    auto& rules = parsed_stylesheet->rules();

    // 3. If rules contains one or more @import rules, remove those rules from rules.
    GC::RootVector<GC::Ref<CSSRule>> rules_without_import(realm().heap());
    for (auto rule : rules) {
        if (rule->type() != CSSRule::Type::Import)
            rules_without_import.append(rule);
    }

    // 4.Set sheet’s CSS rules to rules.
    m_rules->set_rules({}, rules_without_import);

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-addrule
WebIDL::ExceptionOr<WebIDL::Long> CSSStyleSheet::add_rule(Optional<String> selector, Optional<String> style, Optional<WebIDL::UnsignedLong> index)
{
    // 1. Let rule be an empty string.
    StringBuilder rule;

    // 2. Append selector to rule.
    if (selector.has_value())
        rule.append(selector.release_value());

    // 3. Append " { " to rule.
    rule.append('{');

    // 4. If block is not empty, append block, followed by a space, to rule.
    if (style.has_value() && !style->is_empty())
        rule.appendff("{} ", style.release_value());

    // 5. Append "}" to rule.
    rule.append('}');

    // 6. Let index be optionalIndex if provided, or the number of CSS rules in the stylesheet otherwise.
    // 7. Call insertRule(), with rule and index as arguments.
    TRY(insert_rule(rule.string_view(), index.value_or(rules().length())));

    // 8. Return -1.
    return -1;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-removerule
WebIDL::ExceptionOr<void> CSSStyleSheet::remove_rule(Optional<WebIDL::UnsignedLong> index)
{
    // The removeRule(index) method must run the same steps as deleteRule().
    return delete_rule(index.value_or(0));
}

void CSSStyleSheet::for_each_effective_rule(TraversalOrder order, Function<void(Web::CSS::CSSRule const&)> const& callback) const
{
    if (m_media->matches())
        m_rules->for_each_effective_rule(order, callback);
}

void CSSStyleSheet::for_each_effective_style_producing_rule(Function<void(CSSRule const&)> const& callback) const
{
    for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (rule.type() == CSSRule::Type::Style || rule.type() == CSSRule::Type::NestedDeclarations)
            callback(rule);
    });
}

void CSSStyleSheet::for_each_effective_keyframes_at_rule(Function<void(CSSKeyframesRule const&)> const& callback) const
{
    for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (rule.type() == CSSRule::Type::Keyframes)
            callback(static_cast<CSSKeyframesRule const&>(rule));
    });
}

void CSSStyleSheet::add_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
{
    VERIFY(document_or_shadow_root.is_document() || document_or_shadow_root.is_shadow_root());
    m_owning_documents_or_shadow_roots.set(document_or_shadow_root);
}

void CSSStyleSheet::remove_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
{
    m_owning_documents_or_shadow_roots.remove(document_or_shadow_root);
}

void CSSStyleSheet::invalidate_owners(DOM::StyleInvalidationReason reason)
{
    for (auto& document_or_shadow_root : m_owning_documents_or_shadow_roots) {
        document_or_shadow_root->invalidate_style(reason);
        document_or_shadow_root->document().style_computer().invalidate_rule_cache();
    }
}

bool CSSStyleSheet::evaluate_media_queries(HTML::Window const& window)
{
    bool any_media_queries_changed_match_state = false;

    bool now_matches = m_media->evaluate(window);
    if (!m_did_match.has_value() || m_did_match.value() != now_matches)
        any_media_queries_changed_match_state = true;
    if (now_matches && m_rules->evaluate_media_queries(window))
        any_media_queries_changed_match_state = true;

    m_did_match = now_matches;

    return any_media_queries_changed_match_state;
}

Optional<FlyString> CSSStyleSheet::default_namespace() const
{
    if (m_default_namespace_rule)
        return m_default_namespace_rule->namespace_uri();

    return {};
}

Optional<FlyString> CSSStyleSheet::namespace_uri(StringView namespace_prefix) const
{
    return m_namespace_rules.get(namespace_prefix)
        .map([](GC::Ptr<CSSNamespaceRule> namespace_) {
            return namespace_->namespace_uri();
        });
}

void CSSStyleSheet::recalculate_rule_caches()
{
    m_default_namespace_rule = nullptr;
    m_namespace_rules.clear();
    m_import_rules.clear();

    for (auto const& rule : *m_rules) {
        // "Any @import rules must precede all other valid at-rules and style rules in a style sheet
        // (ignoring @charset and @layer statement rules) and must not have any other valid at-rules
        // or style rules between it and previous @import rules, or else the @import rule is invalid."
        // https://drafts.csswg.org/css-cascade-5/#at-import
        //
        // "Any @namespace rules must follow all @charset and @import rules and precede all other
        // non-ignored at-rules and style rules in a style sheet.
        // ...
        // A syntactically invalid @namespace rule (whether malformed or misplaced) must be ignored."
        // https://drafts.csswg.org/css-namespaces/#syntax
        switch (rule->type()) {
        case CSSRule::Type::Import: {
            // @import rules must appear before @namespace rules, so skip this if we've seen @namespace.
            if (!m_namespace_rules.is_empty())
                continue;
            m_import_rules.append(as<CSSImportRule>(*rule));
            break;
        }
        case CSSRule::Type::Namespace: {
            auto& namespace_rule = as<CSSNamespaceRule>(*rule);
            if (!namespace_rule.namespace_uri().is_empty() && namespace_rule.prefix().is_empty())
                m_default_namespace_rule = namespace_rule;

            m_namespace_rules.set(namespace_rule.prefix(), namespace_rule);
            break;
        }
        default:
            // Any other types mean that further @namespace rules are invalid, so we can stop here.
            return;
        }
    }
}

void CSSStyleSheet::set_source_text(String source)
{
    m_source_text = move(source);
}

Optional<String> CSSStyleSheet::source_text(Badge<DOM::Document>) const
{
    return m_source_text;
}

bool CSSStyleSheet::has_associated_font_loader(FontLoader& font_loader) const
{
    for (auto& loader : m_associated_font_loaders) {
        if (loader.ptr() == &font_loader)
            return true;
    }
    return false;
}

}
