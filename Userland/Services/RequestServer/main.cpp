/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/System.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibTLS/Certificate.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/HttpProtocol.h>
#include <RequestServer/HttpsProtocol.h>
#include <signal.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    TRY(Core::System::pledge("stdio inet accept thread unix rpath sendfd recvfd sigaction"));

#ifdef SIGINFO
    signal(SIGINFO, [](int) { RequestServer::ConnectionCache::dump_jobs(); });
#endif

    TRY(Core::System::pledge("stdio inet accept thread unix rpath sendfd recvfd"));

    // Ensure the certificates are read out here.
    // FIXME: Allow specifying extra certificates on the command line, or in other configuration.
    [[maybe_unused]] auto& certs = DefaultRootCACertificates::the();

    Core::EventLoop event_loop;
    // FIXME: Establish a connection to LookupServer and then drop "unix"?
    TRY(Core::System::unveil("/tmp/portal/lookup", "rw"));
    TRY(Core::System::unveil("/etc/cacert.pem", "rw"));
    TRY(Core::System::unveil("/etc/timezone", "r"));
    TRY(Core::System::unveil(nullptr, nullptr));

    RequestServer::HttpProtocol::install();
    RequestServer::HttpsProtocol::install();

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<RequestServer::ConnectionFromClient>());

    return event_loop.exec();
}
