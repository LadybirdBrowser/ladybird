/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/Collator.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(Collator);

// 10 Collator Objects, https://tc39.es/ecma402/#collator-objects
Collator::Collator(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void Collator::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bound_compare);
}

}
