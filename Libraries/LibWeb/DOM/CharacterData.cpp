/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/Editing/WritingSuggestionController.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

CharacterData::RareData::~RareData() = default;

OwnPtr<Node::RareData> CharacterData::create_rare_data() const
{
    return make<RareData>();
}

CharacterData::RareData& CharacterData::ensure_character_data_rare_data() const
{
    return static_cast<RareData&>(ensure_rare_data());
}

CharacterData::RareData* CharacterData::character_data_rare_data()
{
    return static_cast<RareData*>(rare_data());
}

CharacterData::RareData const* CharacterData::character_data_rare_data() const
{
    return static_cast<RareData const*>(rare_data());
}

GC_DEFINE_ALLOCATOR(CharacterData);

static bool ends_in_ascii_whitespace(Utf16String const& data)
{
    return !data.is_empty() && is_ascii_space(data.code_unit_at(data.length_in_code_units() - 1));
}

// Whether white space text may follow the node among the boxes of its parent, with no in-flow box between them.
static bool whitespace_text_may_follow(Node const& node)
{
    for (auto const* sibling = node.next_sibling(); sibling; sibling = sibling->next_sibling()) {
        if (auto const* text = as_if<Text>(*sibling))
            return text->data().is_ascii_whitespace();
        auto const* element = as_if<Element>(*sibling);
        if (!element || !element->has_style())
            continue;
        CSS::ComputedStyleRecordView values { element->installed_style() };
        auto display = values->display();
        // The children of a display: contents element continue the boxes before it.
        if (display.is_contents())
            return true;
        auto is_out_of_flow = values->float_() != CSS::Float::None
            || first_is_one_of(values->position(), CSS::Positioning::Absolute, CSS::Positioning::Fixed);
        if (!display.is_none() && !is_out_of_flow)
            return false;
    }
    return false;
}

CharacterData::CharacterData(Document& document, NodeType type, Utf16String data)
    : Node(document, type)
    , m_character_data_core { .data = move(data) }
{
    // NB: libweb_rust reads a CharacterData node as the CharacterDataPrefix it starts with.
    static_assert(sizeof(CharacterData) == sizeof(RustFFI::CharacterDataPrefix));
    static_assert(offsetof(CharacterData, m_character_data_core) == offsetof(RustFFI::CharacterDataPrefix, core));

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(CharacterData, m_character_data_core) >= sizeof(Node));
}

CharacterData::~CharacterData() = default;

size_t CharacterData::external_memory_size() const
{
    return Node::external_memory_size() + JS::utf16_string_external_memory_size(m_character_data_core.data);
}

// https://dom.spec.whatwg.org/#dom-characterdata-data
void CharacterData::set_data(Utf16View const& data)
{
    // [The data] setter must replace data with node this, offset 0, count this’s length, and data new value.
    // NOTE: Since the offset is 0, it can never be above data's length, so this can never throw.
    // NOTE: Setting the data to the same value as the current data still causes a mutation observer callback.
    // FIXME: Figure out a way to make this a no-op again if the passed in data is the same as the current data.
    MUST(replace_data(0, this->length_in_utf16_code_units(), data, nullptr));
}

void CharacterData::set_data(Utf16String const& data)
{
    // NB: As above, with the new value's string, which the node's data then shares.
    MUST(replace_data(0, this->length_in_utf16_code_units(), data.utf16_view(), &data));
}

// https://dom.spec.whatwg.org/#concept-cd-substring
WebIDL::ExceptionOr<Utf16String> CharacterData::substring_data(size_t offset, size_t count) const
{
    // NB: libweb_rust runs the algorithm, see Rust/src/dom/character_data.rs.
    auto substring = RustFFI::rust_dom_substring_data(this, offset, count);
    if (substring == 0)
        return WebIDL::IndexSizeError::create("Substring offset out of range."_utf16);
    return Utf16String::adopt_raw(substring);
}

// https://dom.spec.whatwg.org/#concept-cd-replace
WebIDL::ExceptionOr<void> CharacterData::replace_data(size_t offset, size_t count, Utf16View const& data)
{
    return replace_data(offset, count, data, nullptr);
}

WebIDL::ExceptionOr<void> CharacterData::replace_data(size_t offset, size_t count, Utf16View const& data, Utf16String const* data_string)
{
    // NB: libweb_rust runs the algorithm, see Rust/src/dom/character_data.rs, and calls back into
    //     RustCharacterDataHooks::character_data_replaced() for what it does beyond the data and the live ranges.
    bool replaced = data.has_latin1_storage()
        ? RustFFI::rust_dom_replace_data(this, offset, count, data.latin1_span().data(), nullptr, data.length_in_code_units(), data_string ? data_string->raw_identity() : 0)
        : RustFFI::rust_dom_replace_data(this, offset, count, nullptr, reinterpret_cast<u16 const*>(data.utf16_span().data()), data.length_in_code_units(), data_string ? data_string->raw_identity() : 0);
    if (!replaced)
        return WebIDL::IndexSizeError::create("Replacement offset out of range."_utf16);
    return {};
}

void RustCharacterDataHooks::character_data_replaced(Node* node, size_t old_raw_data, size_t offset, size_t count, size_t data_length, u8 steps)
{
    auto& character_data = static_cast<CharacterData&>(*node);
    auto old_data = Utf16String::from_raw(old_raw_data);

    // NB: Mutations during a recorded editing command must go through the Editing proxy functions.
    if (steps & RustFFI::CHARACTER_DATA_NOTIFY_EDITING_HISTORY)
        character_data.document().editing_history_if_exists()->notify_dom_mutation();

    // 4. Queue a mutation record of "characterData" for node with null, null, node’s data, « », « », null, and null.
    if (steps & RustFFI::CHARACTER_DATA_QUEUE_MUTATION_RECORD)
        character_data.queue_mutation_record(MutationType::characterData, {}, {}, old_data, {}, {}, nullptr, nullptr);

    // 12. If node’s parent is non-null, then run the children changed steps for node’s parent.
    if (steps & RustFFI::CHARACTER_DATA_RUN_CHILDREN_CHANGED_STEPS) {
        if (auto* parent = character_data.parent()) {
            Node::ChildrenChangedMetadata metadata { Node::ChildrenChangedMetadata::Type::Mutation, character_data };
            parent->children_changed(metadata);
        }
    }

    if (steps & RustFFI::CHARACTER_DATA_DATA_CHANGED)
        character_data.did_replace_data(old_data, offset, count, data_length);
}

AbstractRange* RustCharacterDataHooks::selection_range(Node* document)
{
    if (auto selection = static_cast<Document&>(*document).get_selection())
        return selection->range().ptr();
    return nullptr;
}

// What replacing the data of the node does to its spelling markers, its style and its layout, where the new data is not
// the old data.
void CharacterData::did_replace_data(Utf16String const& old_data, size_t offset, size_t count, size_t data_length)
{
    // A spelling marker covers the text it was flagged on: Data replaced around it moves it, and data replaced inside
    // it ends it.
    if (has_spelling_markers())
        document().spelling_markers().did_replace_data(as<Text>(*this), offset, count, data_length);

    if (auto* controller = document().writing_suggestion_controller_if_exists(); controller && is<Text>(*this))
        controller->did_change_text(as<Text>(*this));

    // A text node holding nothing leaves its parent `:empty`, so data arriving in it or leaving it
    // moves the parent's emptiness without any node being created or destroyed.
    if (is<Text>(*this) && old_data.is_empty() != m_character_data_core.data.is_empty()) {
        if (auto* parent = as_if<Element>(parent_node()))
            CSS::record_element_emptiness_changed(*parent, *this, !old_data.is_empty(), !m_character_data_core.data.is_empty());
    }

    // NB: Called during DOM text mutation, layout is stale.
    if (is<Text>(*this)) {
        CSS::record_text_data_changed(as<Text>(*this));
        auto whitespace_only_changed = old_data.is_ascii_whitespace() != m_character_data_core.data.is_ascii_whitespace();
        Layout::RustFFI::FfiBoxMarks marks {};
        marks.text_data_changed = true;
        mark_box(marks);
        // White space alone may render as nothing, so a text that turns into it or out of it may gain or lose its box.
        // So may white space text after it, which renders nothing after text ending in collapsible white space.
        auto trailing_whitespace_changed = ends_in_ascii_whitespace(old_data) != ends_in_ascii_whitespace(m_character_data_core.data);
        if (is_connected() && (whitespace_only_changed || (trailing_whitespace_changed && whitespace_text_may_follow(*this)))) {
            if (!has_layout_box())
                set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::WhitespaceTextBoxChange);
            else if (whitespace_only_changed)
                set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::CharacterDataReplaceData);
            if (auto* parent = this->parent())
                parent->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::WhitespaceTextBoxChange);
        }
    }

    bump_character_data_version();

    if (auto* rare_data = character_data_rare_data()) {
        if (rare_data->grapheme_segmenter)
            rare_data->grapheme_segmenter->set_segmented_text(m_character_data_core.data);
        // The line segmenter may be the ASCII fast-path variant, which only accepts a subset of inputs; let the
        // lazy getter re-pick the implementation against the new data.
        rare_data->line_segmenter = nullptr;
        if (rare_data->word_segmenter)
            rare_data->word_segmenter->set_segmented_text(m_character_data_core.data);
    }

    if (is<Text>(*this)) {
        if (auto parent = parent_element())
            CSS::Invalidation::invalidate_style_after_text_change_under(*parent);
    }
}

// https://dom.spec.whatwg.org/#dom-characterdata-appenddata
WebIDL::ExceptionOr<void> CharacterData::append_data(Utf16View const& data)
{
    // The appendData(data) method steps are to replace data with node this, offset this’s length, count 0, and data data.
    return replace_data(this->length_in_utf16_code_units(), 0, data);
}

// https://dom.spec.whatwg.org/#dom-characterdata-insertdata
WebIDL::ExceptionOr<void> CharacterData::insert_data(size_t offset, Utf16View const& data)
{
    // The insertData(offset, data) method steps are to replace data with node this, offset offset, count 0, and data data.
    return replace_data(offset, 0, data);
}

// https://dom.spec.whatwg.org/#dom-characterdata-deletedata
WebIDL::ExceptionOr<void> CharacterData::delete_data(size_t offset, size_t count)
{
    // The deleteData(offset, count) method steps are to replace data with node this, offset offset, count count, and data the empty string.
    return replace_data(offset, count, {});
}

Unicode::Segmenter& CharacterData::grapheme_segmenter() const
{
    auto& segmenter = ensure_character_data_rare_data().grapheme_segmenter;
    if (!segmenter) {
        segmenter = document().grapheme_segmenter().clone();
        segmenter->set_segmented_text(m_character_data_core.data);
    }

    return *segmenter;
}

Unicode::Segmenter& CharacterData::line_segmenter() const
{
    auto& segmenter = ensure_character_data_rare_data().line_segmenter;
    if (!segmenter) {
        if (auto ascii = Unicode::Segmenter::try_create_for_ascii_line(m_character_data_core.data.utf16_view())) {
            segmenter = ascii.release_nonnull();
        } else {
            segmenter = document().line_segmenter().clone();
            segmenter->set_segmented_text(m_character_data_core.data);
        }
    }

    return *segmenter;
}

Unicode::Segmenter& CharacterData::word_segmenter() const
{
    auto& segmenter = ensure_character_data_rare_data().word_segmenter;
    if (!segmenter) {
        segmenter = document().word_segmenter().clone();
        segmenter->set_segmented_text(m_character_data_core.data);
    }

    return *segmenter;
}

}
