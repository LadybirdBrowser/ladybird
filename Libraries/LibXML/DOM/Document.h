/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibXML/DOM/DocumentTypeDeclaration.h>
#include <LibXML/DOM/Node.h>
#include <LibXML/Forward.h>

namespace XML {

enum class Version {
    Version10,
    Version11,
};

struct Doctype {
    ByteString type;
    Vector<MarkupDeclaration> markup_declarations;
    Optional<ExternalID> external_id;
};

class Document {
public:
    explicit Document(NonnullOwnPtr<Node> root, Optional<Doctype> doctype, HashMap<Name, ByteString> processing_instructions, Version version)
        : m_root(move(root))
        , m_processing_instructions(move(processing_instructions))
        , m_version(version)
        , m_explicit_doctype(move(doctype))
    {
    }

    Node& root() { return *m_root; }
    Node const& root() const { return *m_root; }

    HashMap<Name, ByteString> const& processing_instructions() const { return m_processing_instructions; }

    Version version() const { return m_version; }

    Optional<Doctype> const& doctype() const { return m_explicit_doctype; }

private:
    NonnullOwnPtr<Node> m_root;
    HashMap<Name, ByteString> m_processing_instructions;
    Version m_version;
    Optional<Doctype> m_explicit_doctype;
};
}
