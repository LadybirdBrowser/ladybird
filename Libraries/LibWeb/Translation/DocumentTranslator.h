/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakHashMap.h>
#include <LibGC/WeakHashSet.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Page/Translation.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Translation {

// Translates a document's text: It splits the text into units the translator takes one at a time, puts each unit's
// translation back where the text came from, and translates the text the page adds or changes while it's translated.
//
// A paragraph's inline elements, e.g. links and emphasis, are kept: Each run of a paragraph's text carries a token
// that the translation carries over to the words that translate it, so each translated word goes back into the
// element its source text came from, even when the translation puts the words in another order.
class DocumentTranslator {
    AK_MAKE_NONCOPYABLE(DocumentTranslator);
    AK_MAKE_NONMOVABLE(DocumentTranslator);
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit DocumentTranslator(DOM::Document&);

    bool is_started() const { return m_source_language.has_value(); }
    void start(String source_language, String target_language);

    // The unit that should be translated next, if any is waiting, and how urgent it is; lower is more urgent.
    struct Candidate {
        u64 unit_id { 0 };
        u64 priority { 0 };
    };
    Optional<Candidate> next_candidate();
    // Hands over the unit, which is then in flight until its translation comes back.
    Web::TranslationUnit take_unit_for_translation(u64 unit_id);
    void did_translate(Web::TranslatedUnit const&);

    // A mutation the page made; the translator's own mutations never get here.
    void did_mutate(DOM::Node& target, Utf16FlyString const& type, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16String> const& old_value);

    bool is_settled() const;

    void visit_edges(GC::Cell::Visitor&);

private:
    // How the translator treats a node it comes across among a container's children.
    enum class NodeRole : u8 {
        // Text, which is translated.
        Text,
        // An element whose content flows inline with the text around it, e.g. a link.
        Inline,
        // An inline element whose text the translator sees but leaves as it is.
        Kept,
        // A node that stays as it is, wherever the translation puts the text around it, e.g. an image.
        Anchored,
        // An element that holds paragraphs of its own, e.g. a block or an inline-block.
        Container,
        // An element whose content isn't translated at all.
        Excluded,
        // A forced line break, which ends a paragraph.
        Break,
    };

    enum class UnitKind : u8 {
        Paragraph,
        Attribute,
        Title,
        OptionText,
    };

    // The text of a paragraph's run, or the element whose text the translator keeps as it is.
    struct Piece {
        GC::Ref<DOM::Node> node;
        // The inline element of the paragraph the piece's node is in, or nothing if it's directly in the container, and
        // that element's index in the paragraph's inline elements.
        GC::Ptr<DOM::Element> owner;
        Optional<size_t> owner_index;
        Utf16String original_text;
        bool keep_as_is { false };
    };

    // A node of a paragraph that isn't translated, e.g. an image, which stays in place among the translated text.
    struct AnchoredNode {
        GC::Ref<DOM::Node> node;
        GC::Ptr<DOM::Element> owner;
        Optional<size_t> owner_index;
        // The last token before the node and the first after it, among the tokens of its owner's text; 0 for none.
        u32 preceding_token { 0 };
        u32 following_token { 0 };
        // Whether the text was set apart from the node by white space, which a translation drops at the ends of runs.
        bool space_before { false };
        bool space_after { false };
    };

    struct Unit {
        UnitKind kind { UnitKind::Paragraph };
        Vector<Web::TranslationRun> runs;
        bool in_flight { false };
        // How urgent the unit is, as of the last time the viewport moved.
        Optional<u64> priority;

        // A paragraph: The container's children it spans, and every node in them that isn't anchored or kept, in tree
        // order, to tell whether the page changed the paragraph before its translation came back.
        GC::Ptr<DOM::Node> container;
        Vector<GC::Ref<DOM::Node>> range;
        Vector<GC::Ref<DOM::Node>> nodes;
        // The piece of each token, at index token - 1.
        Vector<Piece> pieces;
        // Every inline element of the paragraph, in tree order, and the index of each one's parent among them; nothing
        // for a child of the container.
        Vector<GC::Ref<DOM::Element>> inline_elements;
        Vector<Optional<size_t>> inline_element_parents;
        Vector<AnchoredNode> anchored_nodes;

        // An attribute, the title, or an option's text.
        GC::Ptr<DOM::Element> element;
        Optional<Utf16FlyString> attribute;
        Utf16String original_text;

        void visit_edges(GC::Cell::Visitor&);
    };

    class ParagraphBuilder;

    void scan(DOM::Node& root);
    void scan_container(DOM::Node& container);
    void scan_anchored(DOM::Node&);
    void scan_attributes(DOM::Element&);
    void scan_options(DOM::Element& select);
    void scan_title();
    // NB: Units are made in place, rather than built up elsewhere and moved in, since only here does the collector see the
    //     nodes they hold.
    u64 create_unit(UnitKind);
    Unit& unit(u64 id) { return m_units.find(id)->value; }
    bool is_paragraph_in_flight(Vector<GC::Ref<DOM::Node>> const& range) const;
    void schedule_rescan();
    void rescan();
    void forget_units_within(DOM::Node&);

    NodeRole role_of(DOM::Node const&) const;
    bool is_excluded(DOM::Element const&) const;
    bool is_in_excluded_element(DOM::Node const&) const;
    bool is_inline(DOM::Element const&) const;
    bool contains_paragraph_boundary(DOM::Element const&) const;
    bool is_translated_text(DOM::Text const&) const;

    void apply(Unit&, Vector<Web::TranslationRun> const&);
    void apply_paragraph(Unit&, Vector<Web::TranslationRun> const&);
    bool paragraph_is_unchanged(Unit const&) const;
    u64 priority_of(Unit const&) const;

    GC::Ref<DOM::Document> m_document;
    Optional<String> m_source_language;
    Optional<String> m_target_language;

    HashMap<u64, Unit> m_units;

    // The text the translator wrote, which isn't translated again unless the page changes it.
    GC::WeakHashSet<DOM::Text> m_translated_text;
    GC::WeakHashMap<DOM::Element, HashMap<Utf16FlyString, Utf16String>> m_written_attributes;
    // The title's or an option's text.
    GC::WeakHashMap<DOM::Element, Utf16String> m_written_text;
    // The nodes the page added or changed, whose containers get scanned again.
    Vector<GC::Weak<DOM::Node>> m_dirty_nodes;
    bool m_rescan_scheduled { false };
    Optional<CSSPixelRect> m_prioritized_viewport_rect;
    // Set while the translator changes the document, so it doesn't take its own changes for the page's.
    bool m_is_applying { false };
};

}
