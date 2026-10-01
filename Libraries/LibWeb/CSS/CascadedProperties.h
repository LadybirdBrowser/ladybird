/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// A thin shell over the Rust cascaded property store. The store owns the
// entries (values, importance, origin, layer, cascade order); this shell keeps
// the weak stylesheet sources the store cannot hold, one per store-assigned
// slot.
class CascadedProperties final : public RefCounted<CascadedProperties> {
public:
    static NonnullRefPtr<CascadedProperties> create();

    ~CascadedProperties();

    [[nodiscard]] RefPtr<StyleValue const> property(PropertyID) const;
    // For the Rust-driven cascade application: the underlying store, and assignment of the
    // weak stylesheet source for a slot the store handed out. Inline declarations have no sheet.
    ComputedValuesFFI::CascadedPropertyStore* rust_store() { return m_store; }
    void assign_source_slot(u32 slot, RefPtr<StyleSheetState const> source);
    [[nodiscard]] size_t source_slot_count() const { return m_source_slots.size(); }
    [[nodiscard]] RefPtr<StyleSheetState const> source_for_slot(u32 slot) const;

private:
    CascadedProperties();

    ComputedValuesFFI::CascadedPropertyStore* m_store { nullptr };
    Vector<WeakPtr<StyleSheetState const>> m_source_slots;
    mutable HashMap<PropertyID, ValueComparingNonnullRefPtr<StyleValue const>> m_property_cache;
};

}
