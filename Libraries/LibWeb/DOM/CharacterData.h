/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/DOM/ChildNode.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NonDocumentTypeChildNode.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#characterdata
class WEB_API CharacterData
    : public Node
    , public ChildNode<CharacterData>
    , public NonDocumentTypeChildNode<CharacterData> {
    WEB_PLATFORM_OBJECT(CharacterData, Node);
    GC_DECLARE_ALLOCATOR(CharacterData);

public:
    virtual ~CharacterData() override;

    Utf16String const& data() const { return m_data; }
    void set_data(Utf16String const&);

    unsigned length_in_utf16_code_units() const { return m_data.length_in_code_units(); }

    WebIDL::ExceptionOr<Utf16String> substring_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units) const;
    WebIDL::ExceptionOr<void> append_data(Utf16View const&);
    WebIDL::ExceptionOr<void> insert_data(size_t offset_in_utf16_code_units, Utf16View const&);
    WebIDL::ExceptionOr<void> delete_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units);
    WebIDL::ExceptionOr<void> replace_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units, Utf16View const&);

    Unicode::Segmenter& grapheme_segmenter() const;
    Unicode::Segmenter& line_segmenter() const;
    Unicode::Segmenter& word_segmenter() const;

protected:
    CharacterData(Document&, NodeType, Utf16String);

    virtual void initialize(JS::Realm&) override;

private:
    Utf16String m_data;

    mutable OwnPtr<Unicode::Segmenter> m_grapheme_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_line_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_word_segmenter;
};

}
