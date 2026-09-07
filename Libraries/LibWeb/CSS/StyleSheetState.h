/*
 * Copyright (c) 2026-present, the Ladybird developers.
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Utf16View.h>
#include <AK/Weakable.h>
#include <LibGC/Cell.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/RustStyleSheet.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/TraversalOrder.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::ViewTransition {

class ViewTransition;

}

namespace Web::CSS {

class StyleSheetImport;
class StyleScope;
struct StyleCache;

using CSSStyleSheetOptions = Bindings::CSSStyleSheetInit;

WEB_API StyleSheetState* css_style_sheet_from_value(JS::Value);
WEB_API JS::Value css_style_sheet(JS::Realm&, StyleSheetState&);
WEB_API void resolve_css_style_sheet_promise(JS::Realm&, WebIDL::Promise const&, StyleSheetState&);
WEB_API GC::Ref<JS::SyntheticModule> create_css_style_sheet_default_export_module(JS::Realm&, StyleSheetState&, StringView filename);

// Document attachment, resource loading, and style-engine integration for a Rust stylesheet.
// This is not a CSSOM object. Only cssom_sheet() creates its JavaScript-facing facade.
// Documents and import loaders own this state. Back-references are weak; lazy CSSOM
// facades retain the observable owner chain. The parsed graph is owned by Rust.
class WEB_API StyleSheetState final : public RefCounted<StyleSheetState>
    , public Weakable<StyleSheetState> {
public:
    enum class LoadingState : u8 {
        Unloaded,
        Loading,
        Loaded,
        Error,
    };
    static StringView loading_state_name(LoadingState);

    [[nodiscard]] static NonnullRefPtr<StyleSheetState> create(RustRuleList, GC::Ptr<DOM::Document>, RustMediaList, Optional<::URL::URL> location);
    static WebIDL::ExceptionOr<NonnullRefPtr<StyleSheetState>> create_constructed(DOM::Document const&, CSSStyleSheetOptions const& options = {});

    ~StyleSheetState();

    void visit_edges(GC::Cell::Visitor&);
    size_t external_memory_size() const;

    GC::Ptr<CSSRule const> owner_rule() const;
    GC::Ptr<CSSRule> owner_rule();
    StyleSheetImport* owner_import() const { return m_owner_import.ptr(); }
    void set_owner_import(RefPtr<StyleSheetImport>);

    Utf16FlyString type() const { return "text/css"_utf16_fly_string; }

    CSSStyleSheet& cssom_sheet() const;

    RustRuleList const& native_rules() const { return m_native_sheet.rules(); }
    RustStyleSheet const& native_sheet() const { return m_native_sheet; }
    CSSRuleList const& rules() const;
    CSSRuleList& rules() { return const_cast<CSSRuleList&>(std::as_const(*this).rules()); }

    CSSRuleList* css_rules() { return &rules(); }
    CSSRuleList const* css_rules() const { return &rules(); }

    WebIDL::ExceptionOr<unsigned> insert_rule(Utf16View rule, unsigned index);
    WebIDL::ExceptionOr<WebIDL::Long> add_rule(Optional<Utf16String> selector, Optional<Utf16String> style, Optional<WebIDL::UnsignedLong> index);
    WebIDL::ExceptionOr<void> remove_rule(Optional<WebIDL::UnsignedLong> index);
    WebIDL::ExceptionOr<void> delete_rule(unsigned index);

    GC::Ref<WebIDL::Promise> replace(Utf16String text);
    WebIDL::ExceptionOr<void> replace_sync(Utf16View text);

    void for_each_effective_rule_data(TraversalOrder, Function<void(RustRuleView const&, Utf16View)> const&) const;
    // Returns whether the match state of any media queries changed after evaluation.
    bool evaluate_media_queries(DOM::Document const&);
    bool evaluate_media_queries(DOM::Document const&, Parser::ValueParserFFI::NativeStyleSheetMediaEvaluation&);
    void reload_fonts_after_media_query_change();
    void synchronize_fonts_after_rule_change();
    void record_conditions_for_owners();

    HashTable<GC::Ptr<DOM::Node>> const& owning_documents_or_shadow_roots() const { return m_owning_documents_or_shadow_roots; }
    void add_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root);
    void remove_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root);
    void invalidate_owners();
    GC::Ptr<DOM::Document> owning_document() const;
    void set_disabled(bool);
    void for_each_owning_style_scope(Function<void(StyleScope&)> const&) const;
    NonnullRefPtr<StyleCache> shared_single_constructed_sheet_style_cache();

    // Bumped whenever state that shared style caches derive from changes (rule mutations, media match-state flips).
    // Lets sheet-set style cache registry entries detect staleness at lookup time.
    u64 shared_style_cache_generation() const { return m_shared_style_cache_generation; }

    Optional<Utf16FlyString> default_namespace() const;
    RustNamespaceContext declared_namespaces() const;

    Optional<Utf16FlyString> namespace_uri(Utf16View namespace_prefix) const;
    void for_each_namespace(Function<void(Utf16View prefix, Utf16View uri)> const&) const;
    RefPtr<FontFaceState> css_connected_font_face(u64 rule_identity) const;
    void set_css_connected_font_face(u64 rule_identity, NonnullRefPtr<FontFaceState>);
    void remove_css_connected_font_face(u64 rule_identity);
    void disconnect_font_faces_in_rule(RustRule const&);
    void disconnect_font_faces_in_rules(Parser::ValueParserFFI::NativeRuleList const*);

    Vector<NonnullRefPtr<StyleSheetImport>> const& import_rules() const { return m_import_rules; }
    StyleSheetImport* import_for_rule(u64 identity) const;

    Optional<::URL::URL> base_url() const { return m_base_url; }
    void set_base_url(Optional<::URL::URL> base_url) { m_base_url = move(base_url); }

    void register_pending_image_value(ImageStyleValue& value) { m_pending_image_values.append(value); }
    void invalidate_image_resource_registration() { m_needs_image_resource_registration = true; }
    void load_pending_image_resources(DOM::Document&);
    Optional<::URL::URL> style_resource_base_url() const;

    bool constructed() const { return m_native_sheet.flag(RustStyleSheet::Flag::Constructed); }

    GC::Ptr<DOM::Document const> constructor_document() const { return m_constructor_document; }
    void set_constructor_document(GC::Ptr<DOM::Document const> constructor_document) { m_constructor_document = constructor_document; }

    bool disallow_modification() const { return m_native_sheet.flag(RustStyleSheet::Flag::DisallowModification); }

    void set_source_text(Utf16String source) { m_source_text = move(source); }
    Optional<Utf16String> source_text() const { return m_source_text; }

    void add_critical_subresource(StyleSheetImport&);
    void remove_critical_subresource(StyleSheetImport&);
    LoadingState loading_state() const;
    void check_if_loading_completed();

    // The sheet's StyleEngine program handle, one-based, or 0 while it has none.
    [[nodiscard]] SheetID style_engine_sheet_id() const { return m_style_engine_sheet_id; }
    void set_style_engine_sheet_id(SheetID sheet_id) { m_style_engine_sheet_id = sheet_id; }

    DOM::Element* owner_node() { return m_owner_node.ptr(); }
    DOM::Element const* owner_node() const { return m_owner_node.ptr(); }
    void set_owner_node(DOM::Element*);

    Optional<String> href() const;
    Optional<Utf16String> href_for_bindings() const;

    Optional<::URL::URL> location() const { return m_location; }
    void set_location(Optional<::URL::URL> location) { m_location = move(location); }

    Utf16String const& title() const { return m_title; }
    Optional<Utf16String> title_for_bindings() const;
    void set_title(Utf16String title) { m_title = move(title); }

    GC::Ref<MediaList> media() const;
    RustMediaList const& native_media_list() const { return m_native_sheet.media(); }
    void set_media(Utf16View);

    bool is_alternate() const { return m_native_sheet.flag(RustStyleSheet::Flag::Alternate); }
    void set_alternate(bool alternate) { m_native_sheet.set_flag(RustStyleSheet::Flag::Alternate, alternate); }

    bool is_origin_clean() const { return m_native_sheet.flag(RustStyleSheet::Flag::OriginClean); }
    void set_origin_clean(bool origin_clean) { m_native_sheet.set_flag(RustStyleSheet::Flag::OriginClean, origin_clean); }

    bool disabled() const { return m_native_sheet.flag(RustStyleSheet::Flag::Disabled); }

    StyleSheetState* parent_style_sheet() { return m_parent_style_sheet.ptr(); }
    StyleSheetState const* parent_style_sheet() const { return m_parent_style_sheet.ptr(); }
    void set_parent_css_style_sheet(StyleSheetState*);

private:
    StyleSheetState(RustRuleList, GC::Ptr<DOM::Document>, RustMediaList, Optional<::URL::URL> location);

    void recalculate_rule_caches();
    void set_rules(RustRuleList);
    void invalidate_shared_style_cache();
    bool has_document_owner() const;

    void set_constructed(bool constructed) { m_native_sheet.set_flag(RustStyleSheet::Flag::Constructed, constructed); }
    void set_disallow_modification(bool disallow_modification) { m_native_sheet.set_flag(RustStyleSheet::Flag::DisallowModification, disallow_modification); }

    Parser::ParsingParams make_parsing_params() const;

    RustStyleSheet m_native_sheet;
    struct DocumentMediaState {
        explicit DocumentMediaState(DOM::Document const&);
        ~DocumentMediaState();
        GC::Weak<DOM::Document> document;
        Parser::ValueParserFFI::NativeMediaEvaluationState* state;
    };
    Vector<NonnullOwnPtr<DocumentMediaState>> m_document_media_states;
    mutable GC::Weak<CSSStyleSheet> m_cssom_sheet;
    mutable GC::Weak<MediaList> m_media;
    GC::Ptr<DOM::Element> m_owner_node;
    WeakPtr<StyleSheetState> m_parent_style_sheet;
    Optional<::URL::URL> m_location;
    Utf16String m_title;

    Optional<Utf16String> m_source_text;

    mutable GC::Weak<CSSRuleList> m_rules;
    GC::Ptr<DOM::Document> m_parsing_document;
    HashMap<u64, WeakPtr<FontFaceState>> m_css_connected_font_faces;
    Vector<NonnullRefPtr<StyleSheetImport>> m_import_rules;
    HashMap<u64, NonnullRefPtr<StyleSheetImport>> m_imports;

    WeakPtr<StyleSheetImport> m_owner_import;

    Optional<::URL::URL> m_base_url;
    GC::Ptr<DOM::Document const> m_constructor_document;
    HashTable<GC::Ptr<DOM::Node>> m_owning_documents_or_shadow_roots;
    RefPtr<StyleCache> m_shared_single_constructed_sheet_style_cache;
    u64 m_shared_style_cache_generation { 0 };

    Vector<StyleSheetImport&> m_critical_subresources;

    Vector<WeakPtr<ImageStyleValue>> m_pending_image_values;
    bool m_needs_image_resource_registration { true };

    SheetID m_style_engine_sheet_id;
    bool m_visiting_edges { false };
};

}
