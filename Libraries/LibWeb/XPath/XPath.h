/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace XML::FFI {

struct XPathProgram;

}

namespace Web::XPath {

class XPathExpression;
class XPathNSResolver;
class XPathResult;

// An XPath 1.0 expression, compiled and with its namespace prefixes resolved.
class Expression {
    AK_MAKE_NONCOPYABLE(Expression);
    AK_MAKE_NONMOVABLE(Expression);
    AK_ALLOC_WITH_KMALLOC;

public:
    // Throws a SyntaxError if the text isn't an expression, and a NamespaceError if it uses a prefix the resolver
    // doesn't know. What the resolver throws is reported, and the prefix is one it doesn't know.
    static WebIDL::ExceptionOr<NonnullOwnPtr<Expression>> compile(Utf16String const&, GC::Ptr<XPathNSResolver>);
    ~Expression();

    WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type, GC::Ptr<XPathResult>) const;

    // What a step's node test compares names to.
    struct Name {
        Utf16FlyString local_name;
        Optional<Utf16FlyString> namespace_;
    };

private:
    friend class Evaluator;

    Expression() = default;

    XML::FFI::XPathProgram* m_program { nullptr };
    // One per step of the program.
    Vector<Name> m_names;
    // The string literals, by the node they are.
    HashMap<u32, Utf16String> m_strings;
};

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> create_expression(Utf16String const& expression, GC::Ptr<XPathNSResolver> resolver);
WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(Utf16String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> resolver, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result);

}
