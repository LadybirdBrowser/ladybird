/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Transport.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <WebContent/WebUIClientEndpoint.h>
#include <WebContent/WebUIServerEndpoint.h>

namespace WebContent {

class WebUIConnection final : public IPC::ConnectionFromClient<WebUIClientEndpoint, WebUIServerEndpoint> {
public:
    static ErrorOr<NonnullRefPtr<WebUIConnection>> connect(IPC::File, Web::DOM::Document&);
    virtual ~WebUIConnection() override;

    void visit_edges(JS::Cell::Visitor&);

    void received_message_from_web_ui(String const& name, JS::Value data);

private:
    WebUIConnection(IPC::Transport, Web::DOM::Document&);

    virtual void die() override { }
    virtual void send_message(String name, JsonValue data) override;

    GC::Ref<Web::DOM::Document> m_document;
};

}
