/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/HiddenPageSizing.h>

#include <QLayout>

namespace Ladybird {

void size_hidden_page_like(QWidget& page, QWidget const& current)
{
    page.resize(current.size());

    // Resizing a hidden page updates its geometry, but not its children's: The page's layout runs only on its next
    // activation, which Qt defers until the page is shown. Invalidating first makes activate() lay it out now.
    if (auto* layout = page.layout()) {
        layout->invalidate();
        layout->activate();
    }
}

}
