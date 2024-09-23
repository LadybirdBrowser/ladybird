/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Worker/WebWorkerClientEndpoint.h>
#include <LibWeb/Worker/WebWorkerServerEndpoint.h>
#include <WebWorker/Forward.h>
#include <WebWorker/PageHost.h>

namespace WebWorker {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<WebWorkerClientEndpoint, WebWorkerServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override;

    virtual void die() override;

    virtual void close_worker() override;

    void request_file(Web::FileRequest);

    PageHost& page_host() { return *m_page_host; }
    PageHost const& page_host() const { return *m_page_host; }

private:
    explicit ConnectionFromClient(NonnullOwnPtr<Core::LocalSocket>);

    Web::Page& page();
    Web::Page const& page() const;

    virtual void start_dedicated_worker(URL::URL const& url, Web::Bindings::WorkerType const& type, Web::Bindings::RequestCredentials const& credentials, String const& name, Web::HTML::TransferDataHolder const&, Web::HTML::SerializedEnvironmentSettingsObject const&) override;
    virtual void handle_file_return(i32 error, Optional<IPC::File> const& file, i32 request_id) override;

    JS::Handle<PageHost> m_page_host;

    // FIXME: Route console messages to the Browser UI using a ConsoleClient

    HashMap<int, Web::FileRequest> m_requested_files {};
    int last_id { 0 };

    RefPtr<DedicatedWorkerHost> m_worker_host;
};

}
