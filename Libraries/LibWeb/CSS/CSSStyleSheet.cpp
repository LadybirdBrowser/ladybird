/*
 * Copyright (c) 2019-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/CSSStyleSheet.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleSheet);

CSSStyleSheet* css_style_sheet_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<CSSStyleSheet>(&value.as_object()) : nullptr;
}

JS::Value css_style_sheet(JS::Realm& realm, CSSStyleSheet& style_sheet)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { style_sheet });
}

void resolve_css_style_sheet_promise(JS::Realm& realm, WebIDL::Promise const& promise, CSSStyleSheet& style_sheet)
{
    WebIDL::resolve_promise(promise, css_style_sheet(realm, style_sheet));
}

GC::Ref<JS::SyntheticModule> create_css_style_sheet_default_export_module(JS::Realm& realm, CSSStyleSheet& style_sheet, StringView filename)
{
    return JS::SyntheticModule::create_default_export_synthetic_module(realm, css_style_sheet(realm, style_sheet), filename);
}

WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> CSSStyleSheet::create_for_constructor(JS::Object& relevant_global_object, CSSStyleSheetOptions const& options)
{
    auto associated_document = HTML::relevant_window(relevant_global_object).document();
    return create_constructed(*associated_document, options);
}

GC::Ref<CSSStyleSheet> CSSStyleSheet::create(CSSRuleList& rules, MediaList& media, Optional<::URL::URL> location)
{
    return GC::Heap::the().allocate<CSSStyleSheet>(rules, media, move(location));
}

WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> CSSStyleSheet::create_constructed(DOM::Document const& associated_document, CSSStyleSheetOptions const& options)
{
    // 1. Construct a new CSSStyleSheet object sheet.
    auto sheet = create(CSSRuleList::create(), CSS::MediaList::create({}), {});

    // 2. Set sheet’s location to the base URL of the associated Document for the current principal global object.
    sheet->set_location(associated_document.base_url());

    // 3. Set sheet’s stylesheet base URL to the baseURL attribute value from options.
    if (options.base_url.has_value()) {
        Optional<::URL::URL> sheet_location_url;
        if (sheet->location().has_value())
            sheet_location_url = sheet->location().release_value();

        // AD-HOC: This isn't explicitly mentioned in the specification, but multiple modern browsers do this.
        auto url = DOMURL::parse(options.base_url->utf16_view(), sheet_location_url);
        if (!url.has_value())
            return WebIDL::NotAllowedError::create("Constructed style sheets must have a valid base URL"_utf16);

        sheet->set_base_url(url);
    }

    // 4. Set sheet’s parent CSS style sheet to null.
    sheet->set_parent_css_style_sheet(nullptr);

    // 5. Set sheet’s owner node to null.
    sheet->set_owner_node(nullptr);

    // 6. Set sheet’s owner CSS rule to null.
    sheet->set_owner_css_rule(nullptr);

    // 7. Set sheet’s title to the empty string.
    sheet->set_title({});

    // 8. Unset sheet’s alternate flag.
    sheet->set_alternate(false);

    // 9. Set sheet’s origin-clean flag.
    sheet->set_origin_clean(true);

    // 10. Set sheet’s constructed flag.
    sheet->set_constructed(true);

    // 11. Set sheet’s Constructor document to the associated Document for the current global object.
    sheet->set_constructor_document(&associated_document);

    // 12. If the media attribute of options is a string, create a MediaList object from the string and assign it as sheet’s media.
    //     Otherwise, serialize a media query list from the attribute and then create a MediaList object from the resulting string and set it as sheet’s media.
    if (options.media.has<Utf16String>()) {
        sheet->set_media(options.media.get<Utf16String>());
    } else {
        sheet->m_media = *options.media.get<GC::Ref<MediaList>>();
    }

    // 13. If the disabled attribute of options is true, set sheet’s disabled flag.
    if (options.disabled)
        sheet->set_disabled(true);

    // 14. Return sheet
    return sheet;
}

CSSStyleSheet::CSSStyleSheet(CSSRuleList& rules, MediaList& media, Optional<::URL::URL> location)
    : StyleSheet(media)
    , m_rules(&rules)
{
    if (location.has_value())
        set_location(move(location));

    for (auto& rule : *m_rules)
        rule->set_parent_style_sheet(this);

    recalculate_rule_caches();

    m_rules->on_change = [this]() {
        recalculate_rule_caches();
    };
}

CSSStyleSheet::~CSSStyleSheet() = default;

void CSSStyleSheet::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
    visitor.visit(m_owner_css_rule);
    visitor.visit(m_default_namespace_rule);
    visitor.visit(m_constructor_document);
    visitor.visit(m_namespace_rules);
    visitor.visit(m_import_rules);
    visitor.visit(m_owning_documents_or_shadow_roots);
    if (m_shared_single_constructed_sheet_style_cache)
        m_shared_single_constructed_sheet_style_cache->visit_edges(visitor);
    for (auto& subresource : m_critical_subresources)
        subresource.visit_edges(visitor);
}

size_t CSSStyleSheet::external_memory_size() const
{
    auto size = Base::external_memory_size();
    if (m_source_text.has_value())
        size = JS::saturating_add_external_memory_size(size, JS::utf16_string_external_memory_size(*m_source_text));
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_namespace_rules));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_import_rules));
    size = JS::saturating_add_external_memory_size(size, JS::hash_table_external_memory_size(m_owning_documents_or_shadow_roots));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_critical_subresources));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_pending_image_values));
    return size;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-insertrule
WebIDL::ExceptionOr<unsigned> CSSStyleSheet::insert_rule(Utf16View rule, unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call insert_rule() on non-modifiable stylesheets."_utf16);

    // 3. Let parsed rule be the return value of invoking parse a rule with rule.
    auto parsed_rule = parse_css_rule(make_parsing_params(), rule);

    // 4. If parsed rule is a syntax error, return parsed rule.
    if (!parsed_rule)
        return WebIDL::SyntaxError::create("Unable to parse CSS rule."_utf16);

    // 5. If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException.
    if (constructed() && parsed_rule->type() == CSSRule::Type::Import)
        return WebIDL::SyntaxError::create("Can't insert @import rules into a constructed stylesheet."_utf16);

    // 6. Return the result of invoking insert a CSS rule rule in the CSS rules at index.
    auto result = m_rules->insert_a_css_rule(parsed_rule, index, CSSRuleList::Nested::No, declared_namespaces());

    if (!result.is_exception()) {
        // NOTE: The spec doesn't say where to set the parent style sheet, so we'll do it here.
        parsed_rule->set_parent_style_sheet(this);

        record_style_rule_inserted(*parsed_rule);

        // OPTIMIZATION: A style rule or a keyframes rule arriving in a style element's own sheet changes only which
        //               rules the scope holds, so dropping the rule cache is enough. Anything else can change what
        //               the sheet as a whole contributes, which is what invalidate_owners() re-reads.
        auto style_element_sheet = !constructed() && owner_node() && owner_node()->is_html_style_element();
        auto rule_type = parsed_rule->type();
        if (style_element_sheet && (rule_type == CSSRule::Type::Keyframes || rule_type == CSSRule::Type::Style))
            invalidate_rule_cache_for_style_sheet_owners(*this);
        else
            invalidate_owners();

        synchronize_fonts_after_rule_change();
    }

    return result;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-deleterule
WebIDL::ExceptionOr<void> CSSStyleSheet::delete_rule(unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // 2. If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call delete_rule() on non-modifiable stylesheets."_utf16);

    // 3. Remove a CSS rule in the CSS rules at index.
    auto removed_rule = m_rules->item(index);
    auto result = m_rules->remove_a_css_rule(index);
    if (!result.is_exception()) {
        if (removed_rule)
            record_style_rule_removed(*this, *removed_rule);
        invalidate_owners();
        synchronize_fonts_after_rule_change();
    }
    return result;
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replace
GC::Ref<WebIDL::Promise> CSSStyleSheet::replace(Utf16String text)
{
    // Constructed sheets remember the document whose realm created them. A stylesheet attached to a
    // <style> element has no constructor document, so use its owner node's document instead.
    auto& realm = constructed()
        ? HTML::relevant_realm(*constructor_document())
        : HTML::relevant_realm(owner_node()->document());
    auto promise = constructed()
        ? WebIDL::create_promise_for(*constructor_document())
        : WebIDL::create_promise_for(owner_node()->document());
    if (!constructed()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Can't call replace() on non-constructed stylesheets"_utf16));
        return promise;
    }
    if (disallow_modification()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Can't call replace() on non-modifiable stylesheets"_utf16));
        return promise;
    }
    set_disallow_modification(true);
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, text = move(text), promise = GC::Root(promise)] {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let rules be the result of running parse a stylesheet’s contents from text.
        auto rules = CSS::Parser::Parser::create(make_parsing_params(), text).parse_as_stylesheet_contents();
        GC::RootVector<GC::Ref<CSSRule>> rules_without_import;
        for (auto rule : rules) {
            if (rule->type() != CSSRule::Type::Import)
                rules_without_import.append(rule);
        }
        for (auto& rule : rules_without_import)
            rule->set_parent_style_sheet(this);
        m_rules->set_rules({}, rules_without_import);
        record_stylesheet_rules_replaced(*this);
        invalidate_owners();
        synchronize_fonts_after_rule_change();

        // 4. Unset sheet’s disallow modification flag.
        set_disallow_modification(false);
        WebIDL::resolve_promise(*promise, css_style_sheet(realm, *this));
    }));
    return promise;
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replacesync
WebIDL::ExceptionOr<void> CSSStyleSheet::replace_sync(Utf16View text)
{
    // 1. If the constructed flag is not set, or the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (!constructed())
        return WebIDL::NotAllowedError::create("Can't call replaceSync() on non-constructed stylesheets"_utf16);
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call replaceSync() on non-modifiable stylesheets"_utf16);

    // 2. Let rules be the result of running parse a stylesheet’s contents from text.
    auto rules = CSS::Parser::Parser::create(make_parsing_params(), text).parse_as_stylesheet_contents();

    // 3. If rules contains one or more @import rules, remove those rules from rules.
    GC::RootVector<GC::Ref<CSSRule>> rules_without_import;
    for (auto rule : rules) {
        if (rule->type() != CSSRule::Type::Import)
            rules_without_import.append(rule);
    }

    // NOTE: The spec doesn't say where to set the parent style sheet, so we'll do it here.
    for (auto& rule : rules_without_import) {
        rule->set_parent_style_sheet(this);
    }

    // 4. Set sheet’s CSS rules to rules.
    m_rules->set_rules({}, rules_without_import);
    record_stylesheet_rules_replaced(*this);
    invalidate_owners();
    synchronize_fonts_after_rule_change();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-addrule
WebIDL::ExceptionOr<WebIDL::Long> CSSStyleSheet::add_rule(Optional<Utf16String> selector, Optional<Utf16String> style, Optional<WebIDL::UnsignedLong> index)
{
    // 1. Let rule be an empty string.
    Utf16StringBuilder rule;

    // 2. Append selector to rule.
    if (selector.has_value())
        rule.append(selector.release_value());

    // 3. Append " { " to rule.
    rule.append_code_unit(u'{');

    // 4. If block is not empty, append block, followed by a space, to rule.
    if (style.has_value() && !style->is_empty()) {
        rule.append(style.release_value());
        rule.append_code_unit(u' ');
    }

    // 5. Append "}" to rule.
    rule.append_code_unit(u'}');

    // 6. Let index be optionalIndex if provided, or the number of CSS rules in the stylesheet otherwise.
    // 7. Call insertRule(), with rule and index as arguments.
    auto rule_text = rule.to_string();
    TRY(insert_rule(rule_text, index.value_or(rules().length())));

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

void CSSStyleSheet::for_each_effective_keyframes_at_rule(Function<void(CSSKeyframesRule const&)> const& callback) const
{
    for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (rule.type() == CSSRule::Type::Keyframes)
            callback(static_cast<CSSKeyframesRule const&>(rule));
    });
}

void CSSStyleSheet::for_each_effective_counter_style_at_rule(Function<void(CSSCounterStyleRule const&)> const& callback) const
{
    for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (rule.type() == CSSRule::Type::CounterStyle)
            callback(static_cast<CSSCounterStyleRule const&>(rule));
    });
}

void CSSStyleSheet::for_each_effective_function_at_rule(Function<void(CSSFunctionRule const&)> const& callback) const
{
    for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (rule.type() == CSSRule::Type::Function)
            callback(static_cast<CSSFunctionRule const&>(rule));
    });
}

void CSSStyleSheet::add_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
{
    VERIFY(document_or_shadow_root.is_document() || document_or_shadow_root.is_shadow_root());
    auto had_document_owner = has_document_owner();
    m_owning_documents_or_shadow_roots.set(document_or_shadow_root);

    // CSSOM's "add a CSS style sheet" steps bail out once the disabled flag is set, so ownership alone should not
    // make a disabled sheet observable in the destination document. Delay its media-query evaluation and
    // CSS-connected font activation until the sheet actually becomes enabled.
    if (!disabled() && this->owning_documents_or_shadow_roots().size() == 1)
        evaluate_media_queries(document_or_shadow_root.document());

    if (!disabled() && document_or_shadow_root.is_document() && !had_document_owner)
        document_or_shadow_root.document().font_computer().load_fonts_from_sheet(*this);

    for (auto const& import_rule : m_import_rules) {
        if (import_rule->loaded_style_sheet())
            import_rule->loaded_style_sheet()->add_owning_document_or_shadow_root(document_or_shadow_root);
    }
}

void CSSStyleSheet::remove_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
{
    bool is_removing_last_document_owner = false;
    if (document_or_shadow_root.is_document() && m_owning_documents_or_shadow_roots.contains(document_or_shadow_root)) {
        is_removing_last_document_owner = true;
        for (auto& owner : m_owning_documents_or_shadow_roots) {
            if (owner.ptr() != &document_or_shadow_root && owner->is_document()) {
                is_removing_last_document_owner = false;
                break;
            }
        }
    }

    // CSS @font-face rules contribute to the document font source. Shadow-root-only sheets remain CSSOM-visible and
    // still style their tree, but match other browsers by not contributing shadow-scoped @font-face rules there.
    if (!disabled() && is_removing_last_document_owner)
        document_or_shadow_root.document().font_computer().unload_fonts_from_sheet(*this);

    m_owning_documents_or_shadow_roots.remove(document_or_shadow_root);

    for (auto const& import_rule : m_import_rules) {
        if (import_rule->loaded_style_sheet())
            import_rule->loaded_style_sheet()->remove_owning_document_or_shadow_root(document_or_shadow_root);
    }
}

void CSSStyleSheet::set_disabled(bool disabled)
{
    if (this->disabled() == disabled)
        return;

    auto document = owning_document();
    // When a stylesheet is disabled we stop evaluating its media queries, so both the cached top-level match bit
    // and the MediaList's internal state can go stale across viewport changes. Clear the cache for both
    // directions, and eagerly refresh on re-enable so subsequent rule-cache rebuilds see the current media state
    // instead of the pre-disable one.
    m_did_match = {};
    StyleSheet::set_disabled(disabled);

    if (!disabled && document)
        evaluate_media_queries(*document);

    // A disabled sheet contributes nothing, so its declarations have to be taken back wherever they
    // were winning, and given back when it comes round again.
    for (auto& owner : owning_documents_or_shadow_roots())
        record_stylesheet_conditions(*this, *owner, !disabled && media()->matches());

    if (!disabled) {
        if (document) {
            if (has_document_owner())
                document->font_computer().load_fonts_from_sheet(*this);
            load_pending_image_resources(*document);
        }
    } else if (document && has_document_owner()) {
        document->font_computer().unload_fonts_from_sheet(*this);
    }

    invalidate_owners();
}

void CSSStyleSheet::for_each_owning_style_scope(Function<void(StyleScope&)> const& callback) const
{
    for (auto& document_or_shadow_root : m_owning_documents_or_shadow_roots) {
        auto& style_scope = document_or_shadow_root->is_shadow_root()
            ? as<DOM::ShadowRoot>(*document_or_shadow_root).style_scope()
            : document_or_shadow_root->document().style_scope();

        callback(style_scope);
    }
}

NonnullRefPtr<StyleCache> CSSStyleSheet::shared_single_constructed_sheet_style_cache()
{
    VERIFY(constructed());
    if (!m_shared_single_constructed_sheet_style_cache)
        m_shared_single_constructed_sheet_style_cache = StyleCache::create();
    return *m_shared_single_constructed_sheet_style_cache;
}

void CSSStyleSheet::invalidate_shared_style_cache()
{
    m_shared_single_constructed_sheet_style_cache = nullptr;
    ++m_shared_style_cache_generation;

    // Imported rules contribute to their parent sheet's effective rules.
    if (auto* import_rule = as_if<CSSImportRule>(owner_rule().ptr())) {
        if (auto* parent_style_sheet = import_rule->parent_style_sheet())
            parent_style_sheet->invalidate_shared_style_cache();
    }
}

void CSSStyleSheet::invalidate_owners()
{
    auto previously_matched = m_did_match;
    m_did_match = {};
    invalidate_shared_style_cache();

    // The MediaList may have been mutated (e.g. via MediaList::set_media_text), so refresh the media state before
    // reporting what the sheet now says.
    if (auto document = owning_document()) {
        evaluate_media_queries(*document);
        if (previously_matched.has_value() && previously_matched.value() != m_did_match.value()) {
            reload_fonts_after_media_query_change();
            record_conditions_for_owners();
        }
    }

    invalidate_rule_cache_for_style_sheet_owners(*this);
}

void CSSStyleSheet::reload_fonts_after_media_query_change()
{
    synchronize_fonts_after_rule_change();
}

// https://drafts.csswg.org/css-font-loading/#document-font-face-set
// As @font-face rules are added or removed from a stylesheet, or stylesheets containing @font-face rules are added or
// removed, the corresponding CSS-connected FontFace objects must be added or removed from the document's font source,
// and maintain this ordering.
void CSSStyleSheet::synchronize_fonts_after_rule_change()
{
    if (!has_document_owner())
        return;

    if (auto document = owning_document())
        document->font_computer().load_fonts_from_sheet(*this);
}

bool CSSStyleSheet::has_document_owner() const
{
    for (auto& document_or_shadow_root : m_owning_documents_or_shadow_roots) {
        if (document_or_shadow_root->is_document())
            return true;
    }
    return false;
}

GC::Ptr<DOM::Document> CSSStyleSheet::owning_document() const
{
    if (!m_owning_documents_or_shadow_roots.is_empty())
        return (*m_owning_documents_or_shadow_roots.begin())->document();

    if (auto* element = const_cast<CSSStyleSheet*>(this)->owner_node())
        return element->document();

    return nullptr;
}

void CSSStyleSheet::load_pending_image_resources(DOM::Document& document)
{
    if (disabled())
        return;

    auto pending = move(m_pending_image_values);
    for (auto const& weak_image_value : pending) {
        if (auto* image_value = weak_image_value.ptr()) {
            image_value->update_style_sheet_resource_context(*this);
            image_value->load_any_resources(document);
        }
    }
}

// A sheet whose media stopped matching contributes nothing, so its declarations have to be taken
// back wherever they were winning, and given back when it matches again. Publishing the activation
// is separate from noticing the transition, because the two routes into a media change notice it in
// different places: an ordinary re-evaluation compares against the baseline it holds, while
// `invalidate_owners` resets that baseline first and compares for itself.
void CSSStyleSheet::record_conditions_for_owners()
{
    for (auto& owner : owning_documents_or_shadow_roots())
        record_stylesheet_conditions(*this, *owner, media()->matches() && !disabled());
}

bool CSSStyleSheet::evaluate_media_queries(DOM::Document const& document)
{
    return evaluate_media_queries(document, [](CSSRule const&) { });
}

bool CSSStyleSheet::evaluate_media_queries(DOM::Document const& document, Function<void(CSSRule const&)> const& changed_rule_callback)
{
    bool any_media_queries_changed_match_state = false;

    bool now_matches = m_media->evaluate(document);
    // The first evaluation establishes the baseline. The sheet's rules already entered the cascade through
    // StyleSheetListAddSheet, AdoptedStyleSheetsList, or invalidate_owners (each of which performs its own
    // invalidation), so we don't need to also report a match-state change just because no prior result was
    // recorded.
    bool did_match_state_change = m_did_match.has_value() && m_did_match.value() != now_matches;
    if (did_match_state_change)
        any_media_queries_changed_match_state = true;
    if (now_matches && m_rules->evaluate_media_queries(document, changed_rule_callback))
        any_media_queries_changed_match_state = true;
    if (did_match_state_change) {
        // Imported stylesheets can also have cascade effects through their owning @import rule itself, for example
        // when a layered import's media gate starts or stops contributing its layer declaration.
        if (auto rule = owner_rule())
            changed_rule_callback(*rule);
        m_rules->for_each_effective_rule(TraversalOrder::Preorder, changed_rule_callback);
    }

    if (did_match_state_change)
        record_conditions_for_owners();

    m_did_match = now_matches;
    if (any_media_queries_changed_match_state) {
        invalidate_shared_style_cache();
        record_stylesheet_rule_conditions(*this);
    }

    return any_media_queries_changed_match_state;
}

Optional<Utf16FlyString> CSSStyleSheet::default_namespace() const
{
    if (m_default_namespace_rule)
        return m_default_namespace_rule->namespace_uri();

    return {};
}

HashTable<Utf16FlyString> CSSStyleSheet::declared_namespaces() const
{
    HashTable<Utf16FlyString> declared_namespaces;

    for (auto namespace_ : m_namespace_rules.keys()) {
        declared_namespaces.set(namespace_);
    }

    return declared_namespaces;
}

Optional<Utf16FlyString> CSSStyleSheet::namespace_uri(Utf16View namespace_prefix) const
{
    return m_namespace_rules.get(namespace_prefix)
        .map([](GC::Ptr<CSSNamespaceRule> namespace_) {
            return namespace_->namespace_uri();
        });
}

void CSSStyleSheet::recalculate_rule_caches()
{
    invalidate_shared_style_cache();

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
            if (namespace_rule.prefix().is_empty())
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

void CSSStyleSheet::add_critical_subresource(Subresource& subresource)
{
    m_critical_subresources.append(subresource);
}

void CSSStyleSheet::remove_critical_subresource(Subresource& subresource)
{
    m_critical_subresources.remove_first_matching([&](auto const& it) { return &it == &subresource; });
    check_if_loading_completed();
}

CSSStyleSheet::LoadingState CSSStyleSheet::loading_state() const
{
    bool any_loading = false;
    bool any_errors = false;

    for (auto const& subresource : m_critical_subresources) {
        switch (subresource.loading_state()) {
        case LoadingState::Unloaded:
        case LoadingState::Loading:
            any_loading = true;
            break;
        case LoadingState::Loaded:
            break;
        case LoadingState::Error:
            any_errors = true;
            break;
        }
    }

    if (any_loading)
        return LoadingState::Loading;

    if (any_errors)
        return LoadingState::Error;

    return LoadingState::Loaded;
}

void CSSStyleSheet::check_if_loading_completed()
{
    auto state = loading_state();
    if (state == LoadingState::Loaded || state == LoadingState::Error) {
        // We're finished loading, so propagate that to our owner.
        if (auto* style_element = as_if<DOM::StyleElementBase>(owner_node())) {
            style_element->finished_loading_critical_subresources(state == LoadingState::Error ? DOM::StyleElementBase::AnyFailed::Yes : DOM::StyleElementBase::AnyFailed::No);
        } else if (auto* link_element = as_if<HTML::HTMLLinkElement>(owner_node())) {
            link_element->finished_loading_critical_style_subresources(state == LoadingState::Error ? HTML::HTMLLinkElement::AnyFailed::Yes : HTML::HTMLLinkElement::AnyFailed::No);
        } else if (auto* import_rule = as_if<CSSImportRule>(owner_rule().ptr())) {
            import_rule->set_loading_state(state);
        }
    }
}

Parser::ParsingParams CSSStyleSheet::make_parsing_params() const
{
    Parser::ParsingParams parsing_params;
    if (auto document = owning_document())
        parsing_params = Parser::ParsingParams { *document };

    parsing_params.declared_namespaces = declared_namespaces();
    return parsing_params;
}

StringView CSSStyleSheet::loading_state_name(LoadingState loading_state)
{
    switch (loading_state) {
    case LoadingState::Unloaded:
        return "Unloaded"sv;
    case LoadingState::Loading:
        return "Loading"sv;
    case LoadingState::Loaded:
        return "Loaded"sv;
    case LoadingState::Error:
        return "Error"sv;
    }
    VERIFY_NOT_REACHED();
}

void CSSStyleSheet::Subresource::set_loading_state(LoadingState loading_state)
{
    m_loading_state = loading_state;
    if (loading_state == LoadingState::Loaded || loading_state == LoadingState::Error) {
        if (auto style_sheet = parent_style_sheet_for_subresource())
            style_sheet->check_if_loading_completed();
    }
}

}
