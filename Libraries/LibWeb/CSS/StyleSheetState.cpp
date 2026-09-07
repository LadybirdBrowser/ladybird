/*
 * Copyright (c) 2026-present, the Ladybird developers.
 * Copyright (c) 2019-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFaceState.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

NonnullRefPtr<StyleSheetState> StyleSheetState::create(RustRuleList rules, GC::Ptr<DOM::Document> document, RustMediaList media, Optional<::URL::URL> location)
{
    return adopt_ref(*new StyleSheetState(move(rules), document, move(media), move(location)));
}

WebIDL::ExceptionOr<NonnullRefPtr<StyleSheetState>> StyleSheetState::create_constructed(DOM::Document const& associated_document, CSSStyleSheetOptions const& options)
{
    // 1. Construct a new CSSStyleSheet object sheet.
    auto media = options.media.has<GC::Ref<MediaList>>() ? options.media.get<GC::Ref<MediaList>>()->native_list().share() : RustMediaList {};
    auto sheet = create(RustRuleList {}, const_cast<DOM::Document*>(&associated_document), move(media), {});

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
    sheet->set_owner_import(nullptr);

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
    }

    // 13. If the disabled attribute of options is true, set sheet’s disabled flag.
    if (options.disabled)
        sheet->set_disabled(true);

    // 14. Return sheet
    return sheet;
}

StyleSheetState::StyleSheetState(RustRuleList rules, GC::Ptr<DOM::Document> document, RustMediaList media, Optional<::URL::URL> location)
    : m_native_sheet(move(rules), move(media))
    , m_parsing_document(document)
{
    if (location.has_value())
        set_location(move(location));

    recalculate_rule_caches();
}

StyleSheetState::~StyleSheetState() = default;

void StyleSheetState::disconnect_font_faces_in_rule(RustRule const& rule)
{
    if (rule.type() == RustRule::Type::FontFace) {
        if (auto face = css_connected_font_face(rule.identity()))
            face->disconnect_from_css_rule();
    }
    if (auto* children = Parser::ValueParserFFI::rust_rule_children(rule.handle()))
        disconnect_font_faces_in_rules(children);
}

void StyleSheetState::disconnect_font_faces_in_rules(Parser::ValueParserFFI::NativeRuleList const* rules)
{
    Parser::ValueParserFFI::rust_rule_list_visit_font_rules(rules, this, [](void const* context, Parser::ValueParserFFI::NativeRuleView const* view) {
        RustRuleView rule { *view };
        if (rule.type() == RustRule::Type::FontFace) {
            if (auto face = static_cast<StyleSheetState const*>(context)->css_connected_font_face(rule.identity()))
                face->disconnect_from_css_rule();
        }
    });
}

void StyleSheetState::set_rules(RustRuleList rules)
{
    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // If a @font-face rule is removed from the document, its corresponding FontFace object is no longer CSS-connected.
    // The connection is not restorable by any means (but adding the @font-face back to the stylesheet will create a
    // brand new FontFace object which is CSS-connected).
    disconnect_font_faces_in_rules(m_native_sheet.rules().handle());
    if (m_rules)
        m_rules->set_rules({}, move(rules), m_parsing_document);
    else
        m_native_sheet.rules().replace(rules);
}

CSSRuleList const& StyleSheetState::rules() const
{
    if (!m_rules) {
        auto& sheet = const_cast<StyleSheetState&>(*this);
        m_rules = CSSRuleList::create(RustRuleList { Parser::ValueParserFFI::rust_rule_list_retain(m_native_sheet.rules().handle()) }, m_parsing_document);
        m_rules->set_parent_style_sheet(&sheet);
        m_rules->on_change = [&sheet]() { sheet.recalculate_rule_caches(); };
    }
    return *m_rules;
}

GC::Ptr<CSSRule const> StyleSheetState::owner_rule() const
{
    return m_owner_import ? &m_owner_import->cssom_rule() : nullptr;
}

GC::Ptr<CSSRule> StyleSheetState::owner_rule()
{
    return m_owner_import ? &m_owner_import->cssom_rule() : nullptr;
}

void StyleSheetState::set_owner_import(RefPtr<StyleSheetImport> import)
{
    m_owner_import = import;
    if (m_cssom_sheet)
        m_cssom_sheet->update_owner_chain();
}

StyleSheetImport* StyleSheetState::import_for_rule(u64 identity) const
{
    auto import = m_imports.find(identity);
    return import != m_imports.end() ? import->value.ptr() : nullptr;
}

void StyleSheetState::visit_edges(GC::Cell::Visitor& visitor)
{
    if (m_visiting_edges)
        return;
    m_visiting_edges = true;
    ScopeGuard finish = [&] { m_visiting_edges = false; };
    visitor.visit(m_cssom_sheet.ptr());
    visitor.visit(m_owner_node);
    visitor.visit(m_media.ptr());
    visitor.visit(m_rules.ptr());
    visitor.visit(m_parsing_document);
    visitor.visit(m_constructor_document);
    for (auto const& import : m_imports)
        import.value->visit_edges(visitor);
    visitor.visit(m_owning_documents_or_shadow_roots);
    // NB: Weak back-references and font-face connections are traced by their owners.
    visitor.ignore(m_parent_style_sheet);
    visitor.ignore(m_owner_import);
    visitor.ignore(m_css_connected_font_faces);
    // NB: These imports are aliases of entries in m_imports, traced above.
    visitor.ignore(m_import_rules);
    // Critical subresources are non-owning aliases of the imports traced above.
    // Visiting them again would double the traversal at every import depth.
}

size_t StyleSheetState::external_memory_size() const
{
    size_t size = sizeof(*this);
    size = JS::saturating_add_external_memory_size(size, JS::utf16_string_external_memory_size(m_title));
    if (!m_rules)
        size = JS::saturating_add_external_memory_size(size, m_native_sheet.rules().external_memory_size());
    if (m_source_text.has_value())
        size = JS::saturating_add_external_memory_size(size, JS::utf16_string_external_memory_size(*m_source_text));
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_css_connected_font_faces));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_import_rules));
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_imports));
    size = JS::saturating_add_external_memory_size(size, JS::hash_table_external_memory_size(m_owning_documents_or_shadow_roots));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_critical_subresources));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_pending_image_values));
    return size;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-insertrule
WebIDL::ExceptionOr<unsigned> StyleSheetState::insert_rule(Utf16View rule, unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call insert_rule() on non-modifiable stylesheets."_utf16);

    // 3. Let parsed rule be the return value of invoking parse a rule with rule.
    auto parsed_rule = parse_css_rule(make_parsing_params(), rule);

    // 4. If parsed rule is a syntax error, return parsed rule.
    if (!parsed_rule.has_value())
        return WebIDL::SyntaxError::create("Unable to parse CSS rule."_utf16);

    // 5. If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException.
    if (constructed() && parsed_rule->type() == CSSRule::Type::Import)
        return WebIDL::SyntaxError::create("Can't insert @import rules into a constructed stylesheet."_utf16);

    // 6. Return the result of invoking insert a CSS rule rule in the CSS rules at index.
    auto result = CSSRuleList::insert_a_css_rule(m_native_sheet.rules(), *parsed_rule, index, CSSRuleList::Nested::No);

    if (!result.is_exception()) {
        recalculate_rule_caches();
        record_style_rule_inserted(*parsed_rule, *this);

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
WebIDL::ExceptionOr<void> StyleSheetState::delete_rule(unsigned index)
{
    // FIXME: 1. If the origin-clean flag is unset, throw a SecurityError exception.

    // 2. If the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call delete_rule() on non-modifiable stylesheets."_utf16);

    // 3. Remove a CSS rule in the CSS rules at index.
    TRY(CSSRuleList::validate_rule_removal(native_rules(), index));
    auto removed_rule = native_rules().at(index);
    RefPtr<StyleSheetState> detached_import;
    if (auto* import = import_for_rule(removed_rule.identity()))
        detached_import = import->loaded_style_sheet();
    if (m_rules) {
        m_rules->remove_a_css_rule_without_validation({}, index);
    } else {
        disconnect_font_faces_in_rule(removed_rule);
        m_native_sheet.rules().remove(index);
        recalculate_rule_caches();
    }
    record_style_rule_removed(*this, removed_rule, detached_import.ptr());
    invalidate_owners();
    synchronize_fonts_after_rule_change();
    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replace
GC::Ref<WebIDL::Promise> StyleSheetState::replace(Utf16String text)
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
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [sheet = GC::Ref { cssom_sheet() }, &realm, text = move(text), promise = GC::Root(promise)] {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto& state = sheet->state();

        // 1. Let rules be the result of running parse a stylesheet’s contents from text.
        auto rules = CSS::Parser::Parser { state.make_parsing_params() }.parse_as_stylesheet_contents(text);
        rules.remove_imports();
        state.set_rules(move(rules));
        state.invalidate_image_resource_registration();
        record_stylesheet_rules_replaced(state);
        state.invalidate_owners();
        state.synchronize_fonts_after_rule_change();

        // 4. Unset sheet’s disallow modification flag.
        state.set_disallow_modification(false);
        WebIDL::resolve_promise(*promise, css_style_sheet(realm, state));
    }));
    return promise;
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-replacesync
WebIDL::ExceptionOr<void> StyleSheetState::replace_sync(Utf16View text)
{
    // 1. If the constructed flag is not set, or the disallow modification flag is set, throw a NotAllowedError DOMException.
    if (!constructed())
        return WebIDL::NotAllowedError::create("Can't call replaceSync() on non-constructed stylesheets"_utf16);
    if (disallow_modification())
        return WebIDL::NotAllowedError::create("Can't call replaceSync() on non-modifiable stylesheets"_utf16);

    // 2. Let rules be the result of running parse a stylesheet’s contents from text.
    auto rules = CSS::Parser::Parser { make_parsing_params() }.parse_as_stylesheet_contents(text);

    // 3. If rules contains one or more @import rules, remove those rules from rules.
    rules.remove_imports();

    // 4. Set sheet’s CSS rules to rules.
    set_rules(move(rules));
    invalidate_image_resource_registration();

    record_stylesheet_rules_replaced(*this);
    invalidate_owners();
    synchronize_fonts_after_rule_change();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstylesheet-addrule
WebIDL::ExceptionOr<WebIDL::Long> StyleSheetState::add_rule(Optional<Utf16String> selector, Optional<Utf16String> style, Optional<WebIDL::UnsignedLong> index)
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
    TRY(insert_rule(rule_text, index.value_or(m_native_sheet.rules().size())));

    // 8. Return -1.
    return -1;
}

// https://www.w3.org/TR/cssom/#dom-cssstylesheet-removerule
WebIDL::ExceptionOr<void> StyleSheetState::remove_rule(Optional<WebIDL::UnsignedLong> index)
{
    // The removeRule(index) method must run the same steps as deleteRule().
    return delete_rule(index.value_or(0));
}

void StyleSheetState::for_each_effective_rule_data(TraversalOrder order, Function<void(RustRuleView const&, Utf16View)> const& callback) const
{
    Parser::ValueParserFFI::rust_style_sheet_visit_effective_rule_data(
        m_native_sheet.handle(),
        order == TraversalOrder::Preorder ? Parser::ValueParserFFI::FfiRuleTraversalOrder::Preorder : Parser::ValueParserFFI::FfiRuleTraversalOrder::Postorder,
        &callback,
        [](void const* context, Parser::ValueParserFFI::NativeRuleView const* rule, Parser::ValueParserFFI::FfiUtf16View prefix) {
            (*static_cast<Function<void(RustRuleView const&, Utf16View)> const*>(context))(
                RustRuleView { *rule }, { reinterpret_cast<char16_t const*>(prefix.utf16), prefix.length });
        });
}

void StyleSheetState::add_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
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

void StyleSheetState::remove_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root)
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

void StyleSheetState::set_disabled(bool disabled)
{
    if (this->disabled() == disabled)
        return;

    auto document = owning_document();
    // When a stylesheet is disabled we stop evaluating its media queries, so both the cached top-level match bit
    // and the MediaList's internal state can go stale across viewport changes. Clear the cache for both
    // directions, and eagerly refresh on re-enable so subsequent rule-cache rebuilds see the current media state
    // instead of the pre-disable one.
    m_native_sheet.reset_media_state();
    m_native_sheet.set_flag(RustStyleSheet::Flag::Disabled, disabled);

    if (!disabled && document)
        evaluate_media_queries(*document);

    // A disabled sheet contributes nothing, so its declarations have to be taken back wherever they
    // were winning, and given back when it comes round again.
    for (auto& owner : owning_documents_or_shadow_roots())
        record_stylesheet_conditions(*this, *owner, !disabled && native_media_list().matches());

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

void StyleSheetState::for_each_owning_style_scope(Function<void(StyleScope&)> const& callback) const
{
    for (auto& document_or_shadow_root : m_owning_documents_or_shadow_roots) {
        auto& style_scope = document_or_shadow_root->is_shadow_root()
            ? as<DOM::ShadowRoot>(*document_or_shadow_root).style_scope()
            : document_or_shadow_root->document().style_scope();

        callback(style_scope);
    }
}

NonnullRefPtr<StyleCache> StyleSheetState::shared_single_constructed_sheet_style_cache()
{
    VERIFY(constructed());
    if (!m_shared_single_constructed_sheet_style_cache)
        m_shared_single_constructed_sheet_style_cache = StyleCache::create();
    return *m_shared_single_constructed_sheet_style_cache;
}

void StyleSheetState::invalidate_shared_style_cache()
{
    m_shared_single_constructed_sheet_style_cache = nullptr;
    ++m_shared_style_cache_generation;

    // Imported rules contribute to their parent sheet's effective rules.
    if (auto* import_rule = owner_import()) {
        if (auto* parent_style_sheet = import_rule->parent_style_sheet())
            parent_style_sheet->invalidate_shared_style_cache();
    }
}

void StyleSheetState::invalidate_owners()
{
    auto previously_matched = m_native_sheet.media_state();
    m_native_sheet.reset_media_state();
    invalidate_shared_style_cache();

    // The MediaList may have been mutated (e.g. via MediaList::set_media_text), so refresh the media state before
    // reporting what the sheet now says.
    if (auto document = owning_document()) {
        evaluate_media_queries(*document);
        if (previously_matched != RustStyleSheet::MediaState::Unevaluated && previously_matched != m_native_sheet.media_state()) {
            reload_fonts_after_media_query_change();
            record_conditions_for_owners();
        }
    }

    invalidate_rule_cache_for_style_sheet_owners(*this);
}

void StyleSheetState::reload_fonts_after_media_query_change()
{
    synchronize_fonts_after_rule_change();
    // Media evaluation covers the native import graph, but CSS-connected FontFace objects
    // still belong to each loaded sheet's document state.
    for (auto const& import_rule : m_import_rules) {
        if (auto* imported = import_rule->loaded_style_sheet())
            imported->reload_fonts_after_media_query_change();
    }
}

// https://drafts.csswg.org/css-font-loading/#document-font-face-set
// As @font-face rules are added or removed from a stylesheet, or stylesheets containing @font-face rules are added or
// removed, the corresponding CSS-connected FontFace objects must be added or removed from the document's font source,
// and maintain this ordering.
void StyleSheetState::synchronize_fonts_after_rule_change()
{
    if (!has_document_owner())
        return;

    if (auto document = owning_document())
        document->font_computer().load_fonts_from_sheet(*this);
}

bool StyleSheetState::has_document_owner() const
{
    for (auto& document_or_shadow_root : m_owning_documents_or_shadow_roots) {
        if (document_or_shadow_root->is_document())
            return true;
    }
    return false;
}

GC::Ptr<DOM::Document> StyleSheetState::owning_document() const
{
    if (!m_owning_documents_or_shadow_roots.is_empty())
        return (*m_owning_documents_or_shadow_roots.begin())->document();

    if (auto* element = const_cast<StyleSheetState*>(this)->owner_node())
        return element->document();

    return nullptr;
}

Optional<::URL::URL> StyleSheetState::style_resource_base_url() const
{
    if (auto url = base_url(); url.has_value())
        return url;
    if (auto url = location(); url.has_value())
        return url;
    if (auto document = owning_document())
        return HTML::relevant_settings_object(*document).api_base_url();
    return {};
}

void StyleSheetState::load_pending_image_resources(DOM::Document& document)
{
    if (disabled())
        return;

    // Keep image-bearing values alive only until the pending requests below have been started.
    // Their parsed data stays in the native rules; CSSOM wrappers do not own resource bindings.
    struct Context {
        StyleSheetState& sheet;
        Vector<NonnullRefPtr<StyleValue const>> images;
    } context { *this, {} };
    // A constructed sheet can be adopted by many shadow roots in the same document. Discover
    // its images once, unless a rule-list mutation gives it new resources to register.
    if (exchange(m_needs_image_resource_registration, false)) {
        Parser::ValueParserFFI::rust_rule_list_visit_images(m_native_sheet.rules().handle(), &context, [](void* data, void const* value) {
            auto& context = *static_cast<Context*>(data);
            auto image = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value)));
            const_cast<StyleValue&>(*image).set_style_sheet(&context.sheet);
            context.images.append(move(image));
        });
    }

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
void StyleSheetState::record_conditions_for_owners()
{
    for (auto& owner : owning_documents_or_shadow_roots())
        record_stylesheet_conditions(*this, *owner, native_media_list().matches() && !disabled());
}

bool StyleSheetState::evaluate_media_queries(DOM::Document const& document)
{
    Parser::ValueParserFFI::NativeStyleSheetMediaEvaluation result {};
    return evaluate_media_queries(document, result);
}

StyleSheetState::DocumentMediaState::DocumentMediaState(DOM::Document const& document)
    : document(document)
    , state(Parser::ValueParserFFI::rust_media_evaluation_state_create())
{
}

StyleSheetState::DocumentMediaState::~DocumentMediaState()
{
    Parser::ValueParserFFI::rust_media_evaluation_state_free(state);
}

bool StyleSheetState::evaluate_media_queries(DOM::Document const& document, Parser::ValueParserFFI::NativeStyleSheetMediaEvaluation& result)
{
    auto& mutable_document = const_cast<DOM::Document&>(document);
    mutable_document.flush_deferred_style_change_event();
    m_document_media_states.remove_all_matching([](auto const& state) { return !state->document; });
    auto state = m_document_media_states.find_if([&](auto const& state) { return state->document.ptr().ptr() == &document; });
    if (state == m_document_media_states.end()) {
        m_document_media_states.append(make<DocumentMediaState>(document));
        state = m_document_media_states.end() - 1;
    }
    MediaEnvironmentSnapshot environment { document };
    result = Parser::ValueParserFFI::rust_style_sheet_evaluate_media_queries(m_native_sheet.handle(), environment.ffi_environment(), (*state)->state, mutable_document.style_computer().style_engine().rust_handle());
    if (result.sheet_changed)
        record_conditions_for_owners();
    if (result.any_changed) {
        invalidate_shared_style_cache();
        if (owner_import())
            record_stylesheet_rule_conditions(*this, mutable_document);
    }
    return result.any_changed;
}

Optional<Utf16FlyString> StyleSheetState::default_namespace() const
{
    return namespace_uri(u""sv);
}

RustNamespaceContext StyleSheetState::declared_namespaces() const
{
    return RustNamespaceContext { Parser::ValueParserFFI::rust_rule_list_namespace_context(native_rules().handle()) };
}

Optional<Utf16FlyString> StyleSheetState::namespace_uri(Utf16View namespace_prefix) const
{
    Optional<Utf16FlyString> uri;
    for_each_namespace([&](Utf16View prefix, Utf16View namespace_uri) {
        if (prefix == namespace_prefix)
            uri = Utf16FlyString::from_utf16(namespace_uri);
    });
    return uri;
}

void StyleSheetState::for_each_namespace(Function<void(Utf16View, Utf16View)> const& callback) const
{
    Parser::ValueParserFFI::rust_rule_list_visit_namespaces(native_rules().handle(), &callback, [](void const* context, auto prefix, auto uri) {
        auto const& callback = *static_cast<Function<void(Utf16View, Utf16View)> const*>(context);
        callback({ reinterpret_cast<char16_t const*>(prefix.utf16), prefix.length }, { reinterpret_cast<char16_t const*>(uri.utf16), uri.length });
    });
}

RefPtr<FontFaceState> StyleSheetState::css_connected_font_face(u64 rule_identity) const
{
    if (auto font_face = m_css_connected_font_faces.find(rule_identity); font_face != m_css_connected_font_faces.end())
        return font_face->value.strong_ref();
    return nullptr;
}

void StyleSheetState::set_css_connected_font_face(u64 rule_identity, NonnullRefPtr<FontFaceState> font_face)
{
    VERIFY(!m_css_connected_font_faces.contains(rule_identity));
    m_css_connected_font_faces.set(rule_identity, *font_face);
}

void StyleSheetState::remove_css_connected_font_face(u64 rule_identity)
{
    m_css_connected_font_faces.remove(rule_identity);
}

void StyleSheetState::recalculate_rule_caches()
{
    invalidate_image_resource_registration();
    invalidate_shared_style_cache();

    m_import_rules.clear();
    auto previous_imports = move(m_imports);

    auto document = owning_document();
    if (!document)
        document = m_parsing_document;

    auto const& rules = m_native_sheet.rules();
    Vector<u64> import_identities;
    rules.for_each_rule([&](RustRuleView const& rule) {
        if (rule.type() != RustRule::Type::Import)
            return true;
        auto identity = rule.identity();
        import_identities.append(identity);
        if (auto previous = previous_imports.find(identity); previous != previous_imports.end())
            m_imports.set(identity, previous->value);
        else if (auto* wrapper = m_rules ? m_rules->existing_wrapper(identity) : nullptr)
            m_imports.set(identity, as<CSSImportRule>(*wrapper).import());
        else
            m_imports.set(identity, StyleSheetImport::create(rule, document));
        return true;
    });
    for (auto const& entry : previous_imports) {
        if (!m_imports.contains(entry.key) && entry.value->parent_style_sheet() == this)
            entry.value->set_parent_style_sheet(nullptr);
    }
    for (auto identity : import_identities) {
        if (auto* import = import_for_rule(identity); import && import->parent_style_sheet() != this)
            import->set_parent_style_sheet(this);
    }
    bool seen_namespace = false;
    rules.for_each_rule([&](RustRuleView const& rule) {
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
        switch (rule.type()) {
        case RustRule::Type::Import: {
            // @import rules must appear before @namespace rules, so skip this if we've seen @namespace.
            if (seen_namespace)
                return true;
            m_import_rules.append(*import_for_rule(rule.identity()));
            break;
        }
        case RustRule::Type::Namespace: {
            seen_namespace = true;
            break;
        }
        default:
            // Any other types mean that further @namespace rules are invalid, so we can stop here.
            return false;
        }
        return true;
    });
}

void StyleSheetState::add_critical_subresource(StyleSheetImport& subresource)
{
    m_critical_subresources.append(subresource);
}

void StyleSheetState::remove_critical_subresource(StyleSheetImport& subresource)
{
    m_critical_subresources.remove_first_matching([&](auto const& it) { return &it == &subresource; });
    check_if_loading_completed();
}

StyleSheetState::LoadingState StyleSheetState::loading_state() const
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

void StyleSheetState::check_if_loading_completed()
{
    auto state = loading_state();
    if (state == LoadingState::Loaded || state == LoadingState::Error) {
        // We're finished loading, so propagate that to our owner.
        if (auto* style_element = as_if<DOM::StyleElementBase>(owner_node())) {
            style_element->finished_loading_critical_subresources(state == LoadingState::Error ? DOM::StyleElementBase::AnyFailed::Yes : DOM::StyleElementBase::AnyFailed::No);
        } else if (auto* link_element = as_if<HTML::HTMLLinkElement>(owner_node())) {
            link_element->finished_loading_critical_style_subresources(state == LoadingState::Error ? HTML::HTMLLinkElement::AnyFailed::Yes : HTML::HTMLLinkElement::AnyFailed::No);
        } else if (auto* import_rule = owner_import()) {
            import_rule->set_loading_state(state);
        }
    }
}

Parser::ParsingParams StyleSheetState::make_parsing_params() const
{
    Parser::ParsingParams parsing_params;
    if (auto document = owning_document())
        parsing_params = Parser::ParsingParams { *document };

    parsing_params.declared_namespaces = declared_namespaces();
    return parsing_params;
}

StringView StyleSheetState::loading_state_name(LoadingState loading_state)
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

CSSStyleSheet& StyleSheetState::cssom_sheet() const
{
    if (!m_cssom_sheet)
        m_cssom_sheet = CSSStyleSheet::create(const_cast<StyleSheetState&>(*this));
    return *m_cssom_sheet;
}

GC::Ref<MediaList> StyleSheetState::media() const
{
    if (auto* import = owner_import())
        return import->media();
    if (!m_media) {
        m_media = MediaList::create(native_media_list().retain());
        m_media->set_associated_style_sheet(const_cast<StyleSheetState&>(*this));
    }
    return *m_media;
}

void StyleSheetState::set_media(Utf16View text)
{
    native_media_list().set_text(text);
    invalidate_style_sheet_for_media_change(*this);
}

Optional<String> StyleSheetState::href() const
{
    if (m_location.has_value())
        return m_location->to_string();
    return {};
}

Optional<Utf16String> StyleSheetState::href_for_bindings() const
{
    if (auto href = this->href(); href.has_value())
        return utf16_string_from_url_ascii(*href);
    return {};
}

void StyleSheetState::set_owner_node(DOM::Element* element)
{
    m_owner_node = element;
}

void StyleSheetState::set_parent_css_style_sheet(StyleSheetState* parent)
{
    m_parent_style_sheet = parent;
    if (m_cssom_sheet)
        m_cssom_sheet->update_owner_chain();
}

// https://drafts.csswg.org/cssom/#dom-stylesheet-title
Optional<Utf16String> StyleSheetState::title_for_bindings() const
{
    // The title attribute must return the title or null if title is the empty string.
    if (m_title.is_empty())
        return {};

    return m_title;
}

}
