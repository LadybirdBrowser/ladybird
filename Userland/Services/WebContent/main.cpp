/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ImageCodecPluginSerenity.h"
#include <LibAudio/Loader.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/AudioCodecPluginAgnostic.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/EventLoopPluginSerenity.h>
#include <LibWeb/Platform/FontPluginSerenity.h>
#include <LibWeb/WebSockets/WebSocket.h>
#include <LibWebView/RequestServerAdapter.h>
#include <LibWebView/WebSocketClientAdapter.h>
#include <WebContent/ConnectionFromClient.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;
    TRY(Core::System::pledge("stdio recvfd sendfd accept unix rpath thread proc map_fixed"));

    // This must be first; we can't check if /tmp/webdriver exists once we've unveiled other paths.
    auto webdriver_socket_path = ByteString::formatted("{}/webdriver", TRY(Core::StandardPaths::runtime_directory()));
    if (FileSystem::exists(webdriver_socket_path))
        TRY(Core::System::unveil(webdriver_socket_path, "rw"sv));

    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil("/etc/timezone", "r"));
    TRY(Core::System::unveil("/usr/lib", "r"));
    TRY(Core::System::unveil("/tmp/session/%sid/portal/audio", "rw"));
    TRY(Core::System::unveil("/tmp/session/%sid/portal/request", "rw"));
    TRY(Core::System::unveil("/tmp/session/%sid/portal/image", "rw"));
    TRY(Core::System::unveil(nullptr, nullptr));

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPluginSerenity);
    Web::Platform::ImageCodecPlugin::install(*new WebContent::ImageCodecPluginSerenity);
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPluginSerenity);

    Web::Platform::AudioCodecPlugin::install_creation_hook([](auto loader) {
        return Web::Platform::AudioCodecPluginAgnostic::create(move(loader));
    });

    Web::ResourceLoader::initialize(TRY(WebView::RequestServerAdapter::try_create()));
    TRY(Web::Bindings::initialize_main_thread_vm(Web::HTML::EventLoop::Type::Window));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebContent::ConnectionFromClient>());
    return event_loop.exec();
}
