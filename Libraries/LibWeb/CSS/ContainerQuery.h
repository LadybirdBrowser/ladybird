/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Query.h>
#include <LibWeb/CSS/RustQueryHandle.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-conditional-5/#container-rule
class WEB_API ContainerQuery final : public RefCounted<ContainerQuery> {
public:
    static NonnullRefPtr<ContainerQuery> create(RustQueryHandle);

    Utf16String to_string() const;

private:
    explicit ContainerQuery(RustQueryHandle);

    RustQueryHandle m_rust_query_handle;
};

// Document-thread bindings for an immutable Rust container-condition list.
class WEB_API ContainerConditions final : public RefCounted<ContainerConditions> {
public:
    struct Condition {
        Optional<Utf16FlyString> container_name;
        RefPtr<ContainerQuery> container_query;
    };

    static NonnullRefPtr<ContainerConditions> create(Parser::ValueParserFFI::ContainerConditionsData const*);
    ~ContainerConditions();
    Parser::ValueParserFFI::ContainerConditionsData const* handle() const { return m_data; }

    Vector<Condition> const& entries() const;

private:
    explicit ContainerConditions(Parser::ValueParserFFI::ContainerConditionsData const*);
    Parser::ValueParserFFI::ContainerConditionsData const* m_data;
    mutable Optional<Vector<Condition>> m_entries;
};

}
