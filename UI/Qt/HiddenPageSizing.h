/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QWidget>

namespace Ladybird {

// Gives a page that isn't the current one in a stacked widget the current page's geometry, laid out now. The stacked
// layout sizes only its current page, and Qt holds a hidden page's layout (and its resize event) back until the page
// is shown. So without this, a hidden page and everything in it keep whatever geometry they last had — or, for a page
// that's never been shown, Qt's default 100x30 — until the user selects the page.
void size_hidden_page_like(QWidget& page, QWidget const& current);

}
