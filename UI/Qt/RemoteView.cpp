/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

static HashMap<u64, RemoteView*> s_views;

RemoteView::RemoteView(Hub::View::InitialState initial_state)
    : m_view(make<Hub::View>(*this, move(initial_state)))
{
    s_views.set(view_id(), this);
}

RemoteView::~RemoteView()
{
    s_views.remove(view_id());
    m_view->detach_from_host();
}

RemoteView* RemoteView::find(u64 view_id)
{
    return s_views.get(view_id).value_or(nullptr);
}

RemoteView& RemoteView::of(WebView::ViewImplementation const& view)
{
    return static_cast<RemoteView&>(*static_cast<Hub::View const&>(view).host());
}

}
