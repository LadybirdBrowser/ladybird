/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSNamespaceRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNamespaceRule);

CSSNamespaceRule::CSSNamespaceRule(JS::Realm& realm, Optional<FlyString> const& prefix, FlyString namespace_uri)
    : CSSRule(realm, Type::Namespace)
    , m_namespace_uri(move(namespace_uri))
    , m_prefix(prefix.value_or(""_fly_string))
{
}

GC::Ref<CSSNamespaceRule> CSSNamespaceRule::create(JS::Realm& realm, Optional<FlyString> const& prefix, FlyString namespace_uri)
{
    return realm.create<CSSNamespaceRule>(realm, move(prefix), move(namespace_uri));
}

void CSSNamespaceRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSNamespaceRule);
    Base::initialize(realm);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-rule
String CSSNamespaceRule::serialized() const
{
    StringBuilder builder;
    // The literal string "@namespace", followed by a single SPACE (U+0020),
    builder.append("@namespace "sv);

    // followed by the serialization as an identifier of the prefix attribute (if any),
    if (!m_prefix.is_empty()) {
        serialize_an_identifier(builder, m_prefix);
        // followed by a single SPACE (U+0020) if there is a prefix,
        builder.append(" "sv);
    }

    //  followed by the serialization as URL of the namespaceURI attribute,
    serialize_a_url(builder, m_namespace_uri);

    // followed the character ";" (U+003B).
    builder.append(";"sv);

    return MUST(builder.to_string());
}

}
