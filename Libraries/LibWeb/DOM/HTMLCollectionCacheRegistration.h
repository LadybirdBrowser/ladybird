/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Types.h>

namespace Web::DOM {

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

    mutable IntrusiveListNode<HTMLCollectionCacheRegistration> list_node;
    using List = IntrusiveList<&HTMLCollectionCacheRegistration::list_node>;
};

}
