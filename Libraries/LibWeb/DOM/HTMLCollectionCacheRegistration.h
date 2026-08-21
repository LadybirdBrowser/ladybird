/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Types.h>
#include <LibGC/Ptr.h>

namespace Web::DOM {

class HTMLCollection;

class HTMLCollectionCacheRegistration {
public:
    enum class AttributeInvalidationType : u8 {
        None,
        Class,
        Name,
        IdOrName,
        Href,
        FormControls,
        Count,
    };

    using AttributeInvalidationTypes = u32;

    static constexpr AttributeInvalidationTypes attribute_invalidation_type_mask(AttributeInvalidationType type)
    {
        return 1u << to_underlying(type);
    }

    explicit HTMLCollectionCacheRegistration(HTMLCollection& collection)
        : m_collection(collection)
    {
    }

    HTMLCollection& collection() { return m_collection; }

    IntrusiveListNode<HTMLCollectionCacheRegistration> list_node;
    using List = IntrusiveList<&HTMLCollectionCacheRegistration::list_node>;

private:
    GC::RawRef<HTMLCollection> m_collection;
};

}
