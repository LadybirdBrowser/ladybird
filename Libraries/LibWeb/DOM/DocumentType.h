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

// NB: libweb_rust defines, allocates and destroys every DocumentType, see Rust/src/dom/node_objects.rs.
class WEB_API DocumentType final
    : public Node
    , public ChildNode<DocumentType> {
    WEB_WRAPPABLE(DocumentType, Node);

public:
    [[nodiscard]] static GC::Ref<DocumentType> create(Document&);

    virtual Utf16FlyString node_name() const override { return name(); }

    Utf16FlyString const& name() const { return m_document_type_core.name; }
    void set_name(Utf16FlyString const& name) { m_document_type_core.name = name; }

    Utf16String const& public_id() const { return m_document_type_core.public_id; }
    void set_public_id(Utf16View public_id) { m_document_type_core.public_id = Utf16String::from_utf16(public_id); }

    Utf16String const& system_id() const { return m_document_type_core.system_id; }
    void set_system_id(Utf16View system_id) { m_document_type_core.system_id = Utf16String::from_utf16(system_id); }

private:
    friend struct RustNodeObjectHooks;

    explicit DocumentType(VtableProbe);
    virtual ~DocumentType() override = default;

    RustFFI::DocumentTypeCore m_document_type_core;
};

bool is_valid_doctype_name(Utf16View const&);

}
