/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/DirIterator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/RequestServerSiteBindings.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

static constexpr int idle_request_server_timeout_ms = 60'000;

// Entries last accessed in the same time bucket are evicted together.
static constexpr auto cache_access_granularity = AK::Duration::from_seconds(1);

// Leave room for new writes to avoid evicting after every write.
static constexpr u64 cache_eviction_target_percent = 90;

static constexpr auto cache_eviction_timeout = AK::Duration::from_seconds(10);

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
    : m_maximum_site_instances(Application::request_server_options().maximum_site_request_servers)
{
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

    auto site_instance_count = [&] {
        size_t count = 0;
        for (auto const& instance : m_instances) {
            if (instance->m_session.ptr() == &session && instance->site().has_value())
                ++count;
        }
        return count;
    };
    if (site_instance_count() >= m_maximum_site_instances) {
        retire_idle_instances();
        if (site_instance_count() >= m_maximum_site_instances)
            return browser_instance(session);
    }

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
    if (m_dormant_site_caches.has_value() && is_live_cache_path(instance->cache_path()))
        m_dormant_site_caches->remove(instance->cache_path());
    return instance;
}

ErrorOr<void> RequestServerManager::launch_process(RequestServerInstance& instance)
{
    // Private sessions keep nothing on disk. Every RequestServer of a session would open the same cache otherwise.
    auto disk_cache_mode = instance.session().is_private() == IsPrivate::Yes ? HTTPDiskCacheMode::Disabled : Application::request_server_options().http_disk_cache_mode;
    auto control_client = TRY(launch_request_server_process(instance.cache_path(), instance.site(), uses_site_request_servers(), disk_cache_mode));

    auto weak_instance = instance.make_weak_ptr();
    control_client->on_client_disconnected = [weak_instance](int client_id) {
        if (auto instance = weak_instance.strong_ref())
            the().client_disconnected(*instance, client_id);
    };
    control_client->on_request_server_died = [weak_instance] {
        if (auto instance = weak_instance.strong_ref())
            the().did_lose_process(*instance);
    };
    control_client->on_disk_cache_size_changed = [weak_instance](u64 size) {
        if (auto instance = weak_instance.strong_ref())
            the().disk_cache_size_changed(*instance, size);
    };
    Application::the().configure_request_server_control_client(*control_client);

    instance.m_control_client = move(control_client);
    instance.m_ui_client = nullptr;

    auto connection = TRY(connect_new_client(instance, instance.session(), RequestServer::SiteBinding::Unrestricted));
    auto transport = TRY(connection.handle.create_transport());
    auto ui_client = make_ref_counted<Requests::RequestClient>(move(transport));
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

void RequestServerManager::client_disconnected(RequestServerInstance& instance, int client_id)
{
    Application::the().did_disconnect_request_server_client(client_id);
    instance.m_client_ids.remove(client_id);

    if (!instance.site().has_value() || !instance.m_client_ids.is_empty())
        return;

    auto weak_instance = instance.make_weak_ptr();
    instance.m_idle_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(idle_request_server_timeout_ms), [weak_instance] {
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

    auto reconnect = [&](RequestServerSiteBindings& bindings) {
        if (auto result = bindings.reconnect_lost_clients(instance, lost_client_ids); result.is_error()) {
            warnln("Unable to reconnect a process to its RequestServer: {}", result.error());
            VERIFY_NOT_REACHED();
        }
        return IterationDecision::Continue;
    };
    WebContentClient::for_each_client([&](WebContentClient& client) { return reconnect(client.request_server_site_bindings()); });
    WorkerProcessManager::the().for_each_request_server_site_bindings([&](RequestServerSiteBindings& bindings) { return reconnect(bindings); });
}

void RequestServerManager::retire(RequestServerInstance& instance)
{
    instance.m_retired = true;
    // The process exits once its last client is gone; dropping the control connection and the UI client sees to that.
    m_instances.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == &instance; });

    if (m_dormant_site_caches.has_value() && instance.site().has_value() && instance.session().is_private() == IsPrivate::No && !is_live_cache_path(instance.cache_path()))
        m_dormant_site_caches->set(instance.cache_path(), { .last_use = UnixDateTime::now(), .size = instance.m_disk_cache_size });
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
    for (auto& instance : m_instances)
        instance->control_client().async_set_disk_cache_settings(settings);

    evict_least_recently_used_cache_entries_if_needed();
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

// Private sessions keep nothing in the cache directories of their RequestServers.
bool RequestServerManager::is_live_cache_path(ByteString const& path) const
{
    return any_of(m_instances, [&](auto const& instance) {
        return instance->session().is_private() == IsPrivate::No && instance->cache_path() == path;
    });
}

Vector<ByteString> RequestServerManager::dormant_site_cache_paths()
{
    Vector<ByteString> paths;
    Core::DirIterator iterator(sites_directory(), Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        auto path = iterator.next_full_path();
        if (!is_live_cache_path(path))
            paths.append(move(path));
    }
    return paths;
}

static void for_each_file_under(ByteString const& directory, Function<void(ByteString const&, struct stat const&)> const& callback)
{
    Core::DirIterator iterator(directory, Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        auto path = iterator.next_full_path();
        auto stat = Core::File::stat(path);
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
    m_dormant_site_caches.clear();

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

HashMap<ByteString, RequestServerManager::DormantSiteCache>& RequestServerManager::dormant_site_caches()
{
    if (!m_dormant_site_caches.has_value()) {
        HashMap<ByteString, DormantSiteCache> caches;
        for (auto const& path : dormant_site_cache_paths()) {
            DormantSiteCache cache;
            for_each_file_under(path, [&](auto const&, struct stat const& stat) {
                cache.last_use = max(cache.last_use, UnixDateTime::from_seconds_since_epoch(stat.st_mtime));
                cache.size += stat.st_size;
            });
            caches.set(path, cache);
        }
        m_dormant_site_caches = move(caches);
    }
    return *m_dormant_site_caches;
}

u64 RequestServerManager::maximum_disk_cache_size() const
{
    // Apply the per-server free-space limit to the combined caches too.
    auto maximum_size = Application::settings().browsing_data_settings().disk_cache_settings.maximum_size;
    auto disk_space = FileSystem::compute_disk_space(LexicalPath { Application::request_server_options().cache_path });
    if (disk_space.is_error())
        return maximum_size;
    return static_cast<u64>(HTTP::compute_maximum_disk_cache_size(disk_space.value().free_bytes, maximum_size));
}

void RequestServerManager::disk_cache_size_changed(RequestServerInstance& instance, u64 size)
{
    instance.m_disk_cache_size = size;
    evict_least_recently_used_cache_entries_if_needed();
}

void RequestServerManager::evict_least_recently_used_cache_entries_if_needed()
{
    // Size reports after eviction trigger another pass if needed.
    if (Application::request_server_options().http_disk_cache_mode == HTTPDiskCacheMode::Disabled)
        return;
    if (m_cache_eviction_start_time.has_value() && MonotonicTime::now() - *m_cache_eviction_start_time < cache_eviction_timeout)
        return;

    auto maximum_size = maximum_disk_cache_size();

    u64 total_size = 0;
    for (auto const& [path, cache] : dormant_site_caches())
        total_size += cache.size;
    for (auto const& instance : m_instances)
        total_size += instance->m_disk_cache_size;
    if (total_size <= maximum_size)
        return;

    struct Eviction : public RefCounted<Eviction> {
        u64 id { 0 };
        Vector<Requests::CacheAccessBucket> buckets;
        size_t pending { 0 };
        u64 target_size { 0 };
    };
    auto eviction = adopt_ref(*new Eviction);
    eviction->id = ++m_cache_eviction_id;
    eviction->target_size = maximum_size / 100 * cache_eviction_target_percent;
    m_cache_eviction_start_time = MonotonicTime::now();

    auto settle = [eviction] {
        if (--eviction->pending > 0)
            return;

        auto& manager = the();
        if (manager.m_cache_eviction_id != eviction->id)
            return;
        manager.m_cache_eviction_start_time.clear();

        auto& dormant_caches = manager.dormant_site_caches();
        for (auto const& [path, cache] : dormant_caches)
            eviction->buckets.append({ .last_access_time = cache.last_use, .size = cache.size });
        if (eviction->buckets.is_empty())
            return;

        // Keep the most recently used buckets that fit, and at least the most recent one.
        quick_sort(eviction->buckets, [](auto const& a, auto const& b) { return a.last_access_time > b.last_access_time; });
        auto cutoff = eviction->buckets.first().last_access_time;
        u64 kept_size = 0;
        for (auto const& bucket : eviction->buckets) {
            kept_size += bucket.size;
            if (kept_size > eviction->target_size)
                break;
            cutoff = bucket.last_access_time;
        }

        for (auto& instance : manager.m_instances)
            instance->control_client().async_remove_cache_entries_accessed_before(cutoff);

        dormant_caches.remove_all_matching([&](auto const& path, auto const& cache) {
            if (cache.last_use >= cutoff)
                return false;
            (void)FileSystem::remove(path, FileSystem::RecursionMode::Allowed);
            return true;
        });
    };

    eviction->pending = m_instances.size() + 1;
    for (auto& instance : m_instances) {
        instance->control_client().retrieve_cache_access_profile(cache_access_granularity)->when_resolved([eviction, settle](Vector<Requests::CacheAccessBucket> buckets) {
                                                                                              eviction->buckets.extend(move(buckets));
                                                                                              settle();
                                                                                          })
            .when_rejected([settle](Error&) { settle(); });
    }
    settle();
}

}
