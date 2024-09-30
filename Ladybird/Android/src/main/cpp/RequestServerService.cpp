/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdServiceBase.h"
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibTLS/Certificate.h>
#include <RequestServer/ConnectionFromClient.h>

namespace RequestServer {
extern ByteString g_default_certificate_path;
}

// FIXME: Share b/w RequestServer and WebSocket
static ErrorOr<ByteString> find_certificates(StringView serenity_resource_root)
{
    auto cert_path = ByteString::formatted("{}/res/ladybird/cacert.pem", serenity_resource_root);
    if (!FileSystem::exists(cert_path))
        return Error::from_string_literal("Don't know how to load certs!");
    return cert_path;
}

ErrorOr<int> service_main(int ipc_socket)
{
    // Ensure the certificates are read out here.
    Vector<ByteString> certificates = { TRY(find_certificates(s_ladybird_resource_root)) };
    DefaultRootCACertificates::set_default_certificate_paths(certificates.span());
    RequestServer::g_default_certificate_path = certificates.first();
    [[maybe_unused]] auto& certs = DefaultRootCACertificates::the();

    Core::EventLoop event_loop;

    auto socket = TRY(Core::LocalSocket::adopt_fd(ipc_socket));
    auto client = TRY(RequestServer::ConnectionFromClient::try_create(move(socket)));

    return event_loop.exec();
}
