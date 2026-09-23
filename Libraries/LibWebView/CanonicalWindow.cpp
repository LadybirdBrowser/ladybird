/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalWindow.h>

namespace WebView {

NonnullRefPtr<CanonicalWindow> CanonicalWindow::create(NonnullRefPtr<CanonicalSimilarOriginWindowAgent> agent)
{
    return adopt_ref(*new CanonicalWindow(move(agent)));
}

CanonicalWindow::CanonicalWindow(NonnullRefPtr<CanonicalSimilarOriginWindowAgent> agent)
    : m_agent(move(agent))
{
}

CanonicalWindow::~CanonicalWindow() = default;

}
