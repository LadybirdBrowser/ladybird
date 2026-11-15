/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/RegisteredTypefaces.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>

namespace Compositing {

bool RegisteredTypefaces::register_typeface(TypefaceRegistration registration)
{
    return m_typefaces_by_id.set(registration.typeface_id, move(registration.typeface), AK::HashSetExistingEntryBehavior::Keep) == AK::HashSetResult::InsertedNewEntry;
}

void RegisteredTypefaces::unregister_typeface(u64 typeface_id)
{
    m_typefaces_by_id.remove(typeface_id);
}

RefPtr<Gfx::Font const> RegisteredTypefaces::font_described_by(DisplayListFontDescription const& description) const
{
    auto typeface = m_typefaces_by_id.find(description.typeface_id);
    if (typeface == m_typefaces_by_id.end())
        return nullptr;
    if (description.is_invisible)
        return typeface->value->invisible_font(description.point_size, description.variation_settings, {}, description.synthesis);
    return typeface->value->font(description.point_size, description.variation_settings, {}, description.synthesis);
}

}
