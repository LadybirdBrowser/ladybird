/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibCompositing/Export.h>
#include <LibGfx/Forward.h>

namespace Compositing {

class COMPOSITING_API RegisteredTypefaces {
public:
    [[nodiscard]] bool register_typeface(TypefaceRegistration);
    void unregister_typeface(u64 typeface_id);

    RefPtr<Gfx::Font const> font_described_by(DisplayListFontDescription const&) const;

private:
    HashMap<u64, NonnullRefPtr<Gfx::Typeface const>> m_typefaces_by_id;
};

}
