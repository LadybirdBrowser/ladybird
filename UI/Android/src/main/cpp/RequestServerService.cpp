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
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/HttpProtocol.h>
#include <RequestServer/HttpsProtocol.h>
#include <UI/Utilities.h>

ErrorOr<int> service_main(int ipc_socket)
{
    Core::EventLoop event_loop;

    RequestServer::HttpProtocol::install();
    RequestServer::HttpsProtocol::install();

    auto socket = TRY(Core::LocalSocket::adopt_fd(ipc_socket));
    auto client = TRY(RequestServer::ConnectionFromClient::try_create(move(socket)));

    return event_loop.exec();
}
