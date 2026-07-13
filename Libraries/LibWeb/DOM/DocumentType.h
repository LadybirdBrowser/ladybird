/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/DOM/ChildNode.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API DocumentType final
    : public Node
    , public ChildNode<DocumentType> {
    WEB_PLATFORM_OBJECT(DocumentType, Node);
    GC_DECLARE_ALLOCATOR(DocumentType);

public:
    [[nodiscard]] static GC::Ref<DocumentType> create(Document&);

    virtual ~DocumentType() override = default;

    virtual Utf16FlyString node_name() const override { return m_name; }

    Utf16FlyString const& name() const { return m_name; }
    void set_name(Utf16FlyString const& name) { m_name = name; }

    Utf16String const& public_id() const { return m_public_id; }
    void set_public_id(Utf16View public_id) { m_public_id = Utf16String::from_utf16(public_id); }

    Utf16String const& system_id() const { return m_system_id; }
    void set_system_id(Utf16View system_id) { m_system_id = Utf16String::from_utf16(system_id); }

private:
    explicit DocumentType(Document&);

    virtual void initialize(JS::Realm&) override;

    Utf16FlyString m_name;
    Utf16String m_public_id;
    Utf16String m_system_id;
};

bool is_valid_doctype_name(Utf16View const&);

template<>
inline bool Node::fast_is<DocumentType>() const { return is_document_type(); }

}
