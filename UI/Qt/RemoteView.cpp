/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/RemoteView.h>

namespace Ladybird {

RemoteView::RemoteView(Hub::View::InitialState initial_state)
    : m_view(make<Hub::View>(*this, move(initial_state)))
{
}

RemoteView::~RemoteView()
{
    m_view->detach_from_host();
}

RemoteView& RemoteView::of(WebView::ViewImplementation const& view)
{
    return static_cast<RemoteView&>(*static_cast<Hub::View const&>(view).host());
}

}
