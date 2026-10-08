/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/DirIterator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/RequestServerSiteBindings.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

static constexpr int idle_request_server_timeout_ms = 60'000;

// A site's RequestServer gets this share of the cache size the user allows.
static constexpr u64 site_cache_size_divisor = 8;

RequestServerInstance::RequestServerInstance(NonnullRefPtr<BrowsingSession> session, Optional<Utf16String> site, ByteString cache_path)
    : m_session(move(session))
    , m_site(move(site))
    , m_cache_path(move(cache_path))
{
}

RequestServerInstance::~RequestServerInstance() = default;

RequestServerManager& RequestServerManager::the()
{
    return Application::request_server_manager();
}

RequestServerManager::RequestServerManager()
{
    // Without top-level site isolation, one process could need RequestServers for multiple top-level sites.
    if (Application::web_content_options().site_isolation_mode == SiteIsolationMode::TopLevel)
        m_maximum_site_instances = Application::request_server_options().maximum_site_request_servers;
}

RequestServerManager::~RequestServerManager() = default;

ErrorOr<NonnullRefPtr<RequestServerInstance>> RequestServerManager::browser_instance(BrowsingSession& session)
{
    for (auto& instance : m_instances) {
        if (instance->m_session.ptr() == &session && !instance->site().has_value())
            return instance;
    }
    return launch(session, {});
}

ErrorOr<NonnullRefPtr<RequestServerInstance>> RequestServerManager::instance_for_site(BrowsingSession& session, Utf16String const& top_level_site)
{
    if (!uses_site_request_servers())
        return browser_instance(session);

    for (auto& instance : m_instances) {
        if (instance->m_session.ptr() == &session && instance->site() == top_level_site)
            return instance;
    }

    retire_idle_instances();

    size_t site_instance_count = 0;
    for (auto const& instance : m_instances) {
        if (instance->m_session.ptr() == &session && instance->site().has_value())
            ++site_instance_count;
    }
    if (site_instance_count >= m_maximum_site_instances)
        return browser_instance(session);

    return launch(session, top_level_site);
}

RefPtr<RequestServerInstance> RequestServerManager::instance_for_client(int client_id)
{
    for (auto& instance : m_instances) {
        if (instance->has_client(client_id))
            return instance;
        if (instance->m_ui_client && instance->m_ui_client->request_server_client_id() == client_id)
            return instance;
    }
    return nullptr;
}

void RequestServerManager::for_each_instance(Function<IterationDecision(RequestServerInstance&)> const& callback)
{
    for (auto& instance : Vector { m_instances }) {
        if (callback(*instance) == IterationDecision::Break)
            return;
    }
}

ErrorOr<NonnullRefPtr<RequestServerInstance>> RequestServerManager::launch(NonnullRefPtr<BrowsingSession> session, Optional<Utf16String> site)
{
    auto cache_path = site.has_value() ? cache_path_for_site(sites_directory(), *site) : Application::request_server_options().cache_path;
    auto instance = adopt_ref(*new RequestServerInstance(session, move(site), move(cache_path)));
    TRY(launch_process(*instance));
    m_instances.append(instance);
    return instance;
}

ErrorOr<void> RequestServerManager::launch_process(RequestServerInstance& instance)
{
    // Private sessions keep nothing on disk. Every RequestServer of a session would open the same cache otherwise.
    auto disk_cache_mode = instance.session().is_private() == IsPrivate::Yes ? HTTPDiskCacheMode::Disabled : Application::request_server_options().http_disk_cache_mode;
    auto control_client = TRY(launch_request_server_process(instance.cache_path(), instance.site(), disk_cache_mode));

    auto weak_instance = instance.make_weak_ptr();
    control_client->on_client_disconnected = [weak_instance](int client_id) {
        if (auto instance = weak_instance.strong_ref())
            the().client_disconnected(*instance, client_id);
    };
    control_client->on_request_server_died = [weak_instance] {
        if (auto instance = weak_instance.strong_ref())
            the().did_lose_process(*instance);
    };
    Application::the().configure_request_server_control_client(*control_client);

    instance.m_control_client = move(control_client);
    instance.m_ui_client = nullptr;

    if (instance.site().has_value()) {
        auto settings = Application::settings().browsing_data_settings().disk_cache_settings;
        settings.maximum_size /= site_cache_size_divisor;
        instance.m_control_client->async_set_disk_cache_settings(settings);
    }

    auto connection = TRY(connect_new_client(instance, instance.session(), RequestServer::SiteBinding::Unrestricted));
    auto transport = TRY(connection.handle.create_transport());
    auto ui_client = make_ref_counted<Requests::RequestClient>(move(transport));
#ifdef AK_OS_WINDOWS
    auto init_transport_response = ui_client->send_sync<Messages::RequestServer::InitTransport>(Core::System::getpid());
    ui_client->transport().set_peer_pid(init_transport_response->peer_pid());
#endif
    instance.m_ui_client = move(ui_client);

    return {};
}

ErrorOr<RequestServerClientConnection> RequestServerManager::connect_new_client(RequestServerInstance& instance, BrowsingSession& session, RequestServer::SiteBinding site_binding)
{
    auto is_private = session.is_private() == IsPrivate::Yes ? RequestServer::IsPrivate::Yes : RequestServer::IsPrivate::No;

    // Random client IDs can collide across RequestServers. The UI identifies clients by ID alone, so retry collisions.
    for (size_t attempt = 0; attempt < 8; ++attempt) {
        auto response = instance.control_client().send_sync_but_allow_failure<Messages::RequestServerControl::ConnectNewClient>(is_private, site_binding);
        if (!response || response->client_id() < 0)
            return Error::from_string_literal("Failed to connect to RequestServer");

        auto client_id = response->client_id();
        if (Application::the().session_for_request_server_client(client_id))
            continue;

        Application::the().did_connect_request_server_client(client_id, session, site_binding);
        if (site_binding == RequestServer::SiteBinding::Bound) {
            instance.m_client_ids.set(client_id);
            instance.m_idle_timer = nullptr;
        }
        return RequestServerClientConnection { .handle = response->take_handle(), .client_id = client_id };
    }
    return Error::from_string_literal("RequestServer keeps handing out client IDs that are in use");
}

void RequestServerManager::assign_site(WebContentClient& client, Utf16String const& top_level_site)
{
    if (!uses_site_request_servers())
        return;

    auto& bindings = client.request_server_site_bindings();
    RefPtr<RequestServerInstance> current;
    if (bindings.client_id().has_value())
        current = instance_for_client(*bindings.client_id());

    // Top-level site isolation keeps a process on its first site's RequestServer.
    if (current && current->site().has_value()) {
        if (current->site() != top_level_site)
            dbgln("RequestServer: process {} hosting {} also hosts a document under {}", client.pid(), *current->site(), top_level_site);
        return;
    }

    auto target = instance_for_site(client.session(), top_level_site);
    if (target.is_error()) {
        warnln("Unable to launch a RequestServer for {}: {}", top_level_site, target.error());
        return;
    }
    if (target.value().ptr() == current.ptr())
        return;

    auto connection = connect_new_client(*target.value(), client.session(), RequestServer::SiteBinding::Bound);
    if (connection.is_error()) {
        warnln("Unable to connect a process to the RequestServer for {}: {}", top_level_site, connection.error());
        return;
    }

    bindings.did_connect(connection.value().client_id);
    client.async_connect_to_request_server(move(connection.value().handle), connection.value().client_id);
}

void RequestServerManager::client_disconnected(RequestServerInstance& instance, int client_id)
{
    Application::the().did_disconnect_request_server_client(client_id);
    instance.m_client_ids.remove(client_id);

    if (!instance.site().has_value() || !instance.m_client_ids.is_empty())
        return;

    auto weak_instance = instance.make_weak_ptr();
    instance.m_idle_timer = Core::Timer::create_single_shot(idle_request_server_timeout_ms, [weak_instance] {
        auto instance = weak_instance.strong_ref();
        if (instance && instance->m_client_ids.is_empty())
            the().retire(*instance);
    });
    instance.m_idle_timer->start();
}

void RequestServerManager::did_lose_process(RequestServerInstance& instance)
{
    if (Core::EventLoop::current().was_exit_requested())
        return;

    // The process took every client with it; nothing will report their disconnection.
    auto lost_client_ids = move(instance.m_client_ids);
    for (auto client_id : lost_client_ids)
        Application::the().did_disconnect_request_server_client(client_id);
    if (instance.m_ui_client)
        Application::the().did_disconnect_request_server_client(instance.m_ui_client->request_server_client_id());

    if (instance.site().has_value() && lost_client_ids.is_empty()) {
        retire(instance);
        return;
    }

    if (auto result = launch_process(instance); result.is_error()) {
        warnln("\033[31;1mUnable to launch replacement RequestServer: {}\033[0m", result.error());
        VERIFY_NOT_REACHED();
    }

    // Every process that was served by the RequestServer gets a replacement client, bound to the same sites.
    WebContentClient::for_each_client([&](WebContentClient& client) {
        auto& bindings = client.request_server_site_bindings();
        if (!bindings.client_id().has_value() || !lost_client_ids.contains(*bindings.client_id()))
            return IterationDecision::Continue;

        auto connection = connect_new_client(instance, client.session(), RequestServer::SiteBinding::Bound);
        if (connection.is_error()) {
            warnln("Unable to reconnect a process to its RequestServer: {}", connection.error());
            VERIFY_NOT_REACHED();
        }
        bindings.did_connect(connection.value().client_id);
        client.async_connect_to_request_server(move(connection.value().handle), connection.value().client_id);
        return IterationDecision::Continue;
    });

    auto result = WorkerProcessManager::the().reconnect_to_request_server(instance, [&](RequestServerSiteBindings const& bindings) {
        return bindings.client_id().has_value() && lost_client_ids.contains(*bindings.client_id());
    });
    if (result.is_error()) {
        warnln("Unable to reconnect WebWorker processes to RequestServer: {}", result.error());
        VERIFY_NOT_REACHED();
    }
}

void RequestServerManager::retire(RequestServerInstance& instance)
{
    // The process exits once its last client is gone; dropping the control connection and the UI client sees to that.
    m_instances.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == &instance; });
}

void RequestServerManager::retire_idle_instances()
{
    for (auto& instance : Vector { m_instances }) {
        if (instance->site().has_value() && instance->m_client_ids.is_empty())
            retire(*instance);
    }
}

void RequestServerManager::disk_cache_settings_changed()
{
    auto const& settings = Application::settings().browsing_data_settings().disk_cache_settings;
    for (auto& instance : m_instances) {
        auto instance_settings = settings;
        if (instance->site().has_value())
            instance_settings.maximum_size /= site_cache_size_divisor;
        instance->control_client().async_set_disk_cache_settings(instance_settings);
    }
}

void RequestServerManager::dns_settings_changed()
{
    for (auto& instance : m_instances)
        apply_dns_settings(instance->control_client());
}

ByteString RequestServerManager::sites_directory() const
{
    return LexicalPath::join(Application::request_server_options().cache_path, "RequestServer"sv, "Sites"sv).string();
}

ByteString RequestServerManager::cache_path_for_site(ByteString const& sites_directory, Utf16String const& site)
{
    // Keep the site name readable; append a hash to distinguish sites after sanitizing and truncating it.
    StringBuilder name;
    for (auto character : site.to_byte_string()) {
        if (name.length() >= 48)
            break;
        if (is_ascii_alphanumeric(character) || character == '.' || character == '-')
            name.append(character);
        else
            name.append('_');
    }
    name.appendff("-{:08x}", site.hash());
    return LexicalPath::join(sites_directory, name.string_view()).string();
}

Vector<ByteString> RequestServerManager::dormant_site_cache_paths()
{
    Vector<ByteString> paths;
    Core::DirIterator iterator(sites_directory(), Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        auto path = iterator.next_full_path();
        auto is_live = any_of(m_instances, [&](auto const& instance) { return instance->cache_path() == path; });
        if (!is_live)
            paths.append(move(path));
    }
    return paths;
}

static void for_each_file_under(ByteString const& directory, Function<void(ByteString const&, struct stat const&)> const& callback)
{
    Core::DirIterator iterator(directory, Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        auto path = iterator.next_full_path();
        auto stat = Core::System::stat(path);
        if (stat.is_error())
            continue;
        if (S_ISDIR(stat.value().st_mode))
            for_each_file_under(path, callback);
        else
            callback(path, stat.value());
    }
}

static UnixDateTime last_use_of_site_cache(ByteString const& path)
{
    UnixDateTime last_use;
    for_each_file_under(path, [&](auto const&, struct stat const& stat) {
        last_use = max(last_use, UnixDateTime::from_seconds_since_epoch(stat.st_mtime));
    });
    return last_use;
}

NonnullRefPtr<Core::Promise<Requests::CacheSizes>> RequestServerManager::estimate_cache_size_accessed_since(UnixDateTime since)
{
    struct Estimation : public RefCounted<Estimation> {
        Requests::CacheSizes sizes;
        size_t pending { 0 };
        NonnullRefPtr<Core::Promise<Requests::CacheSizes>> promise { Core::Promise<Requests::CacheSizes>::construct() };
    };
    auto estimation = adopt_ref(*new Estimation);

    for (auto const& path : dormant_site_cache_paths()) {
        for_each_file_under(path, [&](auto const&, struct stat const& stat) {
            estimation->sizes.total += stat.st_size;
            if (UnixDateTime::from_seconds_since_epoch(stat.st_mtime) >= since)
                estimation->sizes.since_requested_time += stat.st_size;
        });
    }

    auto settle = [estimation] {
        if (--estimation->pending == 0)
            estimation->promise->resolve(estimation->sizes);
    };

    estimation->pending = m_instances.size() + 1;
    for (auto& instance : m_instances) {
        instance->control_client().estimate_cache_size_accessed_since(since)->when_resolved([estimation, settle](Requests::CacheSizes sizes) {
                                                                                estimation->sizes.total += sizes.total;
                                                                                estimation->sizes.since_requested_time += sizes.since_requested_time;
                                                                                settle();
                                                                            })
            .when_rejected([settle](Error&) { settle(); });
    }
    settle();

    return estimation->promise;
}

NonnullRefPtr<Core::Promise<Empty>> RequestServerManager::clear_cache(UnixDateTime since)
{
    struct Clearing : public RefCounted<Clearing> {
        size_t pending { 0 };
        NonnullRefPtr<Core::Promise<Empty>> promise { Core::Promise<Empty>::construct() };
    };
    auto clearing = adopt_ref(*new Clearing);

    // Dormant sites lose their whole cache if any file was used since the cutoff.
    for (auto const& path : dormant_site_cache_paths()) {
        if (last_use_of_site_cache(path) >= since)
            (void)FileSystem::remove(path, FileSystem::RecursionMode::Allowed);
    }

    auto settle = [clearing] {
        if (--clearing->pending == 0)
            clearing->promise->resolve({});
    };

    clearing->pending = m_instances.size() + 1;
    for (auto& instance : m_instances) {
        instance->control_client().clear_cache(since)->when_resolved([settle](Empty) { settle(); }).when_rejected([settle](Error&) { settle(); });
    }
    settle();

    return clearing->promise;
}

}
