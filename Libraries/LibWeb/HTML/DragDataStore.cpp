/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

NonnullRefPtr<DragDataStore> DragDataStore::create()
{
    return adopt_ref(*new DragDataStore());
}

DragDataStore::DragDataStore()
    : m_allowed_effects_state(DataTransferEffect::uninitialized)
{
}

DragDataStore::~DragDataStore() = default;

bool DragDataStore::has_text_item() const
{
    for (auto const& item : m_item_list) {
        if (item.kind == DragDataStoreItem::Kind::Text && item.type_string == "text/plain"sv)
            return true;
    }

    return false;
}

}
