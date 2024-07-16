/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/CharacterDataPrototype.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(CharacterData);

CharacterData::CharacterData(Document& document, NodeType type, String const& data)
    : Node(document, type)
    , m_data(data)
{
}

CharacterData::~CharacterData() = default;

void CharacterData::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CharacterData);
}

// https://dom.spec.whatwg.org/#dom-characterdata-data
void CharacterData::set_data(String const& data)
{
    // [The data] setter must replace data with node this, offset 0, count this’s length, and data new value.
    // NOTE: Since the offset is 0, it can never be above data's length, so this can never throw.
    // NOTE: Setting the data to the same value as the current data still causes a mutation observer callback.
    // FIXME: Figure out a way to make this a no-op again if the passed in data is the same as the current data.
    MUST(replace_data(0, this->length_in_utf16_code_units(), data));
}

// https://dom.spec.whatwg.org/#concept-cd-substring
WebIDL::ExceptionOr<String> CharacterData::substring_data(size_t offset, size_t count) const
{
    // 1. Let length be node’s length.
    // FIXME: This is very inefficient!
    auto utf16_data = MUST(AK::utf8_to_utf16(m_data));
    Utf16View utf16_view { utf16_data };
    auto length = utf16_view.length_in_code_units();

    // 2. If offset is greater than length, then throw an "IndexSizeError" DOMException.
    if (offset > length)
        return WebIDL::IndexSizeError::create(realm(), "Substring offset out of range."_fly_string);

    // 3. If offset plus count is greater than length, return a string whose value is the code units from the offsetth code unit
    //    to the end of node’s data, and then return.
    if (offset + count > length)
        return MUST(utf16_view.substring_view(offset).to_utf8());

    // 4. Return a string whose value is the code units from the offsetth code unit to the offset+countth code unit in node’s data.
    return MUST(utf16_view.substring_view(offset, count).to_utf8());
}

// https://dom.spec.whatwg.org/#concept-cd-replace
WebIDL::ExceptionOr<void> CharacterData::replace_data(size_t offset, size_t count, String const& data)
{
    // 1. Let length be node’s length.
    // FIXME: This is very inefficient!
    auto utf16_data = MUST(AK::utf8_to_utf16(m_data));
    Utf16View utf16_view { utf16_data };
    auto length = utf16_view.length_in_code_units();

    // 2. If offset is greater than length, then throw an "IndexSizeError" DOMException.
    if (offset > length)
        return WebIDL::IndexSizeError::create(realm(), "Replacement offset out of range."_fly_string);

    // 3. If offset plus count is greater than length, then set count to length minus offset.
    if (offset + count > length)
        count = length - offset;

    // 4. Queue a mutation record of "characterData" for node with null, null, node’s data, « », « », null, and null.
    queue_mutation_record(MutationType::characterData, {}, {}, m_data, {}, {}, nullptr, nullptr);

    // 5. Insert data into node’s data after offset code units.
    // 6. Let delete offset be offset + data’s length.
    // 7. Starting from delete offset code units, remove count code units from node’s data.
    StringBuilder builder;
    builder.append(MUST(utf16_view.substring_view(0, offset).to_utf8(Utf16View::AllowInvalidCodeUnits::Yes)));
    builder.append(data);
    builder.append(MUST(utf16_view.substring_view(offset + count).to_utf8(Utf16View::AllowInvalidCodeUnits::Yes)));
    m_data = MUST(builder.to_string());

    // 8. For each live range whose start node is node and start offset is greater than offset but less than or equal to offset plus count, set its start offset to offset.
    for (auto& range : Range::live_ranges()) {
        if (range->start_container() == this && range->start_offset() > offset && range->start_offset() <= (offset + count))
            TRY(range->set_start(*range->start_container(), offset));
    }

    // 9. For each live range whose end node is node and end offset is greater than offset but less than or equal to offset plus count, set its end offset to offset.
    for (auto& range : Range::live_ranges()) {
        if (range->end_container() == this && range->end_offset() > offset && range->end_offset() <= (offset + count))
            TRY(range->set_end(*range->end_container(), offset));
    }

    // 10. For each live range whose start node is node and start offset is greater than offset plus count, increase its start offset by data’s length and decrease it by count.
    for (auto& range : Range::live_ranges()) {
        if (range->start_container() == this && range->start_offset() > (offset + count))
            TRY(range->set_start(*range->start_container(), range->start_offset() + data.bytes().size() - count));
    }

    // 11. For each live range whose end node is node and end offset is greater than offset plus count, increase its end offset by data’s length and decrease it by count.
    for (auto& range : Range::live_ranges()) {
        if (range->end_container() == this && range->end_offset() > (offset + count))
            TRY(range->set_end(*range->end_container(), range->end_offset() + data.bytes().size() - count));
    }

    // 12. If node’s parent is non-null, then run the children changed steps for node’s parent.
    if (parent())
        parent()->children_changed();

    // NOTE: Since the text node's data has changed, we need to invalidate the text for rendering.
    //       This ensures that the new text is reflected in layout, even if we don't end up
    //       doing a full layout tree rebuild.
    if (auto* layout_node = this->layout_node(); layout_node && layout_node->is_text_node())
        static_cast<Layout::TextNode&>(*layout_node).invalidate_text_for_rendering();

    document().just_typed_into_input_box(this); // Schedules fast path layout update

    if (m_segmenter)
        m_segmenter->set_segmented_text(m_data);

    return {};
}

// https://dom.spec.whatwg.org/#dom-characterdata-appenddata
WebIDL::ExceptionOr<void> CharacterData::append_data(String const& data)
{
    // The appendData(data) method steps are to replace data with node this, offset this’s length, count 0, and data data.
    return replace_data(this->length_in_utf16_code_units(), 0, data);
}

// https://dom.spec.whatwg.org/#dom-characterdata-insertdata
WebIDL::ExceptionOr<void> CharacterData::insert_data(size_t offset, String const& data)
{
    // The insertData(offset, data) method steps are to replace data with node this, offset offset, count 0, and data data.
    return replace_data(offset, 0, data);
}

// https://dom.spec.whatwg.org/#dom-characterdata-deletedata
WebIDL::ExceptionOr<void> CharacterData::delete_data(size_t offset, size_t count)
{
    // The deleteData(offset, count) method steps are to replace data with node this, offset offset, count count, and data the empty string.
    return replace_data(offset, count, String {});
}

Unicode::Segmenter& CharacterData::segmenter()
{
    if (!m_segmenter) {
        m_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);
        m_segmenter->set_segmented_text(m_data);
    }

    return *m_segmenter;
}

}
