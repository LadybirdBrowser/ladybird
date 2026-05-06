/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdServiceBase.h"
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibTLS/TLSv12.h>
#include <LibWebView/Utilities.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

// Defined here to satisfy the linker for Android shared library builds.
// main.cpp defines this for non-Android builds, but is not compiled into requestserverservice.so.
OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

ErrorOr<int> service_main(int ipc_socket)
{

    RequestServer::set_default_certificate_path(ByteString::formatted("{}/cacert.pem", WebView::s_ladybird_resource_root));

    Core::EventLoop event_loop;

    auto socket = TRY(Core::LocalSocket::adopt_fd(ipc_socket));
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    Optional<HTTP::DiskCache&> disk_cache;
    [[maybe_unused]] auto client = RequestServer::ConnectionFromClient::construct(
        make<IPC::Transport>(move(socket)),
        RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes,
        connections,
        disk_cache);

    return event_loop.exec();
}
