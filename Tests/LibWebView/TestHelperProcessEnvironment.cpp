/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Process.h>

TEST_CASE(helper_environment_keeps_only_allowed_variables)
{
    MUST(Core::Environment::set("GITHUB_TOKEN"sv, "secret"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LADYBIRD_TEST_SWITCH"sv, "1"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LIBWEB_TEST_SWITCH"sv, "1"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LC_ALL"sv, "en_US.UTF-8"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("HOME"sv, "/Users/test"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("https_proxy"sv, "http://proxy:3128"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("NO_PROXY"sv, "localhost"sv, Core::Environment::Overwrite::Yes));

    auto environment = WebView::Process::helper_process_environment(WebView::ProcessType::WebContent);
    EXPECT(environment.contains_slow("LADYBIRD_TEST_SWITCH=1"sv));
    EXPECT(environment.contains_slow("LIBWEB_TEST_SWITCH=1"sv));
    EXPECT(environment.contains_slow("LC_ALL=en_US.UTF-8"sv));
    EXPECT(environment.contains_slow("HOME=/Users/test"sv));
    EXPECT(!environment.contains_slow("GITHUB_TOKEN=secret"sv));

    // Only RequestServer talks to the network, so only it learns about proxies.
    EXPECT(!environment.contains_slow("https_proxy=http://proxy:3128"sv));
    auto request_server_environment = WebView::Process::helper_process_environment(WebView::ProcessType::RequestServer);
    EXPECT(request_server_environment.contains_slow("https_proxy=http://proxy:3128"sv));
    EXPECT(request_server_environment.contains_slow("NO_PROXY=localhost"sv));
    EXPECT(request_server_environment.contains_slow("LADYBIRD_TEST_SWITCH=1"sv));
    EXPECT(!request_server_environment.contains_slow("GITHUB_TOKEN=secret"sv));
}

TEST_CASE(helper_environment_keeps_loader_and_session_variables_out)
{
    MUST(Core::Environment::set("LD_PRELOAD"sv, "/tmp/evil.so"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LD_LIBRARY_PATH"sv, "/tmp"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("DBUS_SESSION_BUS_ADDRESS"sv, "unix:path=/run/user/1000/bus"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("OPENSSL_CONF"sv, "/tmp/openssl.cnf"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_RUNTIME_DIR"sv, "/run/user/1000"sv, Core::Environment::Overwrite::Yes));

    for (auto type : { WebView::ProcessType::Compositor, WebView::ProcessType::WebContent, WebView::ProcessType::WebWorker, WebView::ProcessType::RequestServer, WebView::ProcessType::ImageDecoder, WebView::ProcessType::WasmCompiler }) {
        auto environment = WebView::Process::helper_process_environment(type);
        EXPECT(!environment.contains_slow("LD_PRELOAD=/tmp/evil.so"sv));
        EXPECT(!environment.contains_slow("LD_LIBRARY_PATH=/tmp"sv));
        EXPECT(!environment.contains_slow("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus"sv));
        EXPECT(!environment.contains_slow("OPENSSL_CONF=/tmp/openssl.cnf"sv));
        EXPECT(environment.contains_slow("XDG_RUNTIME_DIR=/run/user/1000"sv));
    }
}

TEST_CASE(helper_environment_gives_each_helper_only_its_own_configuration)
{
    MUST(Core::Environment::set("PULSE_SERVER"sv, "unix:/run/user/1000/pulse/native"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("MESA_SHADER_CACHE_DIR"sv, "/tmp/mesa"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("VK_ICD_FILENAMES"sv, "/usr/share/vulkan/icd.d/radeon_icd.json"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("SSL_CERT_FILE"sv, "/etc/ssl/cert.pem"sv, Core::Environment::Overwrite::Yes));

    // Only the renderer that hosts a Window plays audio.
    auto web_content_environment = WebView::Process::helper_process_environment(WebView::ProcessType::WebContent);
    EXPECT(web_content_environment.contains_slow("PULSE_SERVER=unix:/run/user/1000/pulse/native"sv));
    EXPECT(!web_content_environment.contains_slow("MESA_SHADER_CACHE_DIR=/tmp/mesa"sv));
    EXPECT(!web_content_environment.contains_slow("SSL_CERT_FILE=/etc/ssl/cert.pem"sv));
    auto web_worker_environment = WebView::Process::helper_process_environment(WebView::ProcessType::WebWorker);
    EXPECT(!web_worker_environment.contains_slow("PULSE_SERVER=unix:/run/user/1000/pulse/native"sv));

    // Only the Compositor talks to the GPU driver.
    auto compositor_environment = WebView::Process::helper_process_environment(WebView::ProcessType::Compositor);
    EXPECT(compositor_environment.contains_slow("MESA_SHADER_CACHE_DIR=/tmp/mesa"sv));
    EXPECT(compositor_environment.contains_slow("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json"sv));
    EXPECT(!compositor_environment.contains_slow("PULSE_SERVER=unix:/run/user/1000/pulse/native"sv));

    // Only RequestServer verifies certificates.
    auto request_server_environment = WebView::Process::helper_process_environment(WebView::ProcessType::RequestServer);
    EXPECT(request_server_environment.contains_slow("SSL_CERT_FILE=/etc/ssl/cert.pem"sv));
    EXPECT(!request_server_environment.contains_slow("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json"sv));
}
