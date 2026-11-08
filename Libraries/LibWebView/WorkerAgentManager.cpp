/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibIPC/File.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerAgentManager.h>

namespace WebView {

WorkerAgentManager& WorkerAgentManager::the()
{
    static auto& manager = *new WorkerAgentManager;
    return manager;
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
WorkerAgentManager::StartedWorkerAgent WorkerAgentManager::start_worker_agent(WebContentClient& owner_client, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request)
{
    auto owner = Owner {
        .client = owner_client,
        .page_id = page_id,
        .token = request.owner_token,
    };
    auto is_private = owner_client.is_private();

    // The owner names outside settings, which must be an environment it holds.
    // FIXME: The worker takes the rest of the outside settings, such as their top-level origin, policy container and
    //        cross-origin isolated capability, as the owner's process gave them.
    if (!outside_settings.has_value())
        return {};
    request.outside_settings.origin = outside_settings->origin();

    // NB: The process of a dedicated worker's owner starts it, and only registers it here.
    if (request.agent_type != Web::HTML::AgentType::SharedWorker)
        return {};

    // NB: A worker shares the ancestors of the environment that creates it.
    auto has_cross_site_ancestor = page_environment_has_cross_site_ancestor(owner_client, page_id, *outside_settings);

    // 9. Let outsideStorageKey be the result of running obtain a storage key for non-storage purposes given
    //    outsideSettings.
    auto outside_storage_key = obtain_a_storage_key_for_non_storage_purposes(*outside_settings);

    // 11.1. Let workerGlobalScope be null.
    SharedWorkerKey key {
        .is_private = is_private,
        .storage_key = outside_storage_key,
        .url = request.url,
        .name = Utf16String::from_utf8(request.name),
        .top_level_site = network_isolation_top_level_site(*outside_settings),
        .has_cross_site_ancestor = has_cross_site_ancestor,
    };

    // 11.2. For each scope in the list of all SharedWorkerGlobalScope objects: if workerStorageKey
    //       equals outsideStorageKey, scope's closing flag is false, scope's constructor URL equals
    //       urlRecord, and scope's name equals options["name"], then set workerGlobalScope to scope
    //       and break.
    if (auto existing_agent_id = m_shared_workers.get(key); existing_agent_id.has_value()) {
        auto maybe_agent = m_agents.find(*existing_agent_id);
        if (maybe_agent != m_agents.end()) {
            auto& agent = maybe_agent->value;
            if (!agent.closing) {
                // FIXME: 11.3. If workerGlobalScope is not null, but the user agent has been
                //              configured to disallow communication between the worker represented
                //              by the workerGlobalScope and the scripts whose settings object is
                //              outsideSettings, then set workerGlobalScope to null.

                // 11.4.  If workerGlobalScope is not null, and any of the following are true:
                //        workerGlobalScope's type is not equal to options["type"];
                //        workerGlobalScope's credentials is not equal to options["credentials"]; or
                //        workerGlobalScope's extended lifetime is not equal to
                //        options["extendedLifetime"], then queue a global task on the DOM
                //        manipulation task source given worker's relevant global object to fire an
                //        event named error at worker and abort these steps.
                // 11.5.3. If workerIsSecureContext is not callerIsSecureContext, queue a global task
                //         on the DOM manipulation task source given worker's relevant global object
                //         to fire an event named error at worker and abort these steps.
                // AD-HOC: The owner fires the error itself when it gets no agent back; the spec's
                //         queue-global-task happens inside WorkerAgentParent in the requesting process.
                if (agent.worker_type != request.type
                    || agent.credentials != request.credentials
                    || agent.extended_lifetime != request.extended_lifetime
                    || (agent.worker_is_secure_context.has_value() && *agent.worker_is_secure_context != request.caller_is_secure_context)) {
                    return {};
                }

                // 11.5.8. Append the relevant owner to add given outsideSettings to
                //         workerGlobalScope's owner set.
                // AD-HOC: The browser-side mirror lives in `agent.owners`; the worker-process
                //         owner_set() is appended inside connect_shared_worker_impl (which also
                //         handles steps 11.5.5-11.5.7).
                agent.owners.append(owner);
                agent.extended_lifetime_timer = nullptr;
                agent.client->async_connect_shared_worker(move(request.outside_port), request.outside_settings);
                return { agent.id, {}, agent.client->host() == &owner_client };
            }
        }

        m_shared_workers.remove(key);
    }

    if (has_reached_worker_limit(is_private, *outside_settings))
        return {};

    // 11.6. Otherwise, in parallel, run a worker given worker, urlRecord, outsideSettings, outsidePort,
    //       and options.
    // AD-HOC: A shared worker's thread is in the process of the owner that starts it, if that process may host it, and
    //         otherwise in a process of its own.
    auto agent_id = ++m_next_agent_id;
    RefPtr<WebContentClient> host;
    if (owner_process_may_host_shared_worker(owner, *outside_settings))
        host = owner_client;
    bool const host_has_only_this_worker = !host;
    Optional<IPC::TransportHandle> thread_transport;
    auto client_or_error = [&]() -> ErrorOr<NonnullRefPtr<WebWorkerClient>> {
        // NB: Only the transport of the worker goes on the shared IO thread. The others here have owners of their own.
        auto transport = TRY([] {
            IPC::ScopedSharedIOThread shared_io_thread;
            return IPC::Transport::create_paired();
        }());
        if (host_has_only_this_worker) {
            host = TRY(Application::the().launch_web_content_process_without_page(is_private));
            host->async_start_worker_thread(move(transport.remote_handle), request.agent_type);
        } else {
            thread_transport = move(transport.remote_handle);
        }
        return create_worker_client(move(transport.local), is_private, agent_id, *host, owner_client.request_server_site_bindings());
    }();
    // A worker the system cannot give a thread fails to load, as one whose script cannot be fetched does.
    if (client_or_error.is_error()) {
        dbgln("Unable to start a worker: {}", client_or_error.error());
        if (host_has_only_this_worker && host)
            host->close_server_if_unused();
        return {};
    }
    auto client = client_or_error.release_value();

    Vector<Owner> owners;
    owners.append(owner);

    // AD-HOC: Seed worker_is_secure_context with the caller's value so reuse requests arriving before
    //         the worker finishes loading still get a mismatch check.
    //         worker_did_finish_loading_script overwrites this with the worker's actual value (which
    //         inherits from outside settings, so should match).
    WorkerAgent agent {
        .id = agent_id,
        .client = client,
        .worker_type = request.type,
        .credentials = request.credentials,
        .extended_lifetime = request.extended_lifetime,
        .extended_lifetime_timer = {},
        .worker_is_secure_context = request.caller_is_secure_context,
        .shared_worker_key = move(key),
        .owners = move(owners),
        .outside_settings_owner = owner,
    };
    m_shared_workers.set(agent.shared_worker_key, agent_id);
    m_agents.set(agent_id, move(agent));

    start_worker(*client, move(request), *outside_settings, has_cross_site_ancestor, owner_client.may_read_local_files(), host_has_only_this_worker);

    // NB: The task manager lists a process with the site of its pages. One that has none left only runs on for its shared
    //     workers, so it goes by the site of their origin.
    if (auto process = Application::process_manager().find_child(client->child_id()); process.has_value()) {
        if (auto const& worker_origin = client->inside_settings()->origin(); !worker_origin.is_opaque() && !worker_origin.is_file_origin())
            process->set_site_url(URL::Parser::basic_parse(worker_origin.serialize()));
    }

    return { agent_id, move(thread_transport), !host_has_only_this_worker };
}

void WorkerAgentManager::register_dedicated_worker(WebContentClient& owner_client, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request, IPC::TransportHandle worker_transport)
{
    if (!outside_settings.has_value())
        return;
    DedicatedWorkerOwner owner {
        .is_private = owner_client.is_private(),
        .process = owner_client,
        .request_server_site_bindings = owner_client.request_server_site_bindings(),
        .may_read_local_files = owner_client.may_read_local_files(),
        .has_cross_site_ancestor = page_environment_has_cross_site_ancestor(owner_client, page_id, *outside_settings),
    };
    register_dedicated_worker(owner, *outside_settings, move(request), move(worker_transport));
}

void WorkerAgentManager::register_dedicated_worker(WebWorkerClient& owner_client, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request, IPC::TransportHandle worker_transport)
{
    auto host = owner_client.host();
    if (!outside_settings.has_value() || !host)
        return;
    DedicatedWorkerOwner owner {
        .is_private = owner_client.is_private(),
        .process = *host,
        .request_server_site_bindings = owner_client.request_server_site_bindings(),
        .may_read_local_files = owner_client.may_read_local_files(),
        .has_cross_site_ancestor = static_cast<CanonicalWorkerEnvironmentSettingsObject const&>(*outside_settings).has_cross_site_ancestor(),
    };
    register_dedicated_worker(owner, *outside_settings, move(request), move(worker_transport));
}

// NB: A worker that is not taken up here loses its connection before it starts, and its owner fires an error at it.
void WorkerAgentManager::register_dedicated_worker(DedicatedWorkerOwner const& owner, CanonicalEnvironmentSettingsObject const& outside_settings, Web::HTML::WorkerAgentStartRequest request, IPC::TransportHandle worker_transport)
{
    if (request.agent_type != Web::HTML::AgentType::DedicatedWorker)
        return;
    request.outside_settings.origin = outside_settings.origin();

    if (has_reached_worker_limit(owner.is_private, outside_settings))
        return;

    auto client_or_error = [&]() -> ErrorOr<NonnullRefPtr<WebWorkerClient>> {
        auto transport = TRY([&] {
            IPC::ScopedSharedIOThread shared_io_thread;
            return worker_transport.create_transport();
        }());
        return create_worker_client(move(transport), owner.is_private, 0, owner.process, owner.request_server_site_bindings);
    }();
    if (client_or_error.is_error()) {
        dbgln("Unable to start a dedicated worker: {}", client_or_error.error());
        return;
    }

    auto client = client_or_error.release_value();
    start_worker(*client, move(request), outside_settings, owner.has_cross_site_ancestor, owner.may_read_local_files, false);
}

bool WorkerAgentManager::page_environment_has_cross_site_ancestor(WebContentClient& client, Web::PageId page_id, CanonicalEnvironmentSettingsObject const& environment)
{
    auto* page = client.page(page_id);
    return !page || page->hosted_environment_has_cross_site_ancestor(environment.id()).value_or(true);
}

// NB: A top-level site may run only so many workers at once, wherever its documents and workers start them. An opaque
//     top-level origin is a site of its own. A worker counts until its thread is gone, and one whose creator's browsing
//     context has been discarded, leaving it no top-level origin, does not start.
bool WorkerAgentManager::has_reached_worker_limit(IsPrivate is_private, CanonicalEnvironmentSettingsObject const& outside_settings) const
{
    auto top_level_origin = outside_settings.top_level_origin();
    if (!top_level_origin.has_value())
        return true;
    size_t worker_count_of_top_level_site = 0;
    for (auto const& client : m_clients) {
        auto const& worker_top_level_origin = client->top_level_origin();
        if (client->is_private() == is_private && worker_top_level_origin.has_value() && worker_top_level_origin->is_same_site(*top_level_origin))
            ++worker_count_of_top_level_site;
    }
    return worker_count_of_top_level_site >= Web::HTML::maximum_worker_count;
}

ErrorOr<NonnullRefPtr<WebWorkerClient>> WorkerAgentManager::create_worker_client(NonnullOwnPtr<IPC::Transport> transport, IsPrivate is_private, Web::HTML::WorkerAgentId agent_id, WebContentClient& host, RequestServerSiteBindings const& owner_request_server_site_bindings)
{
    auto client = TRY(try_make_ref_counted<WebWorkerClient>(move(transport), is_private, agent_id));
    client->set_host(host);

    // The worker's RequestServer client uses the cookies of the session the worker itself belongs to.
    auto session = client->session();
    if (!session)
        session = Application::session_for_new_view(is_private);
    // The worker makes its requests for the sites its owner makes them for.
    client->request_server_site_bindings().bind_sites_of(owner_request_server_site_bindings);

    // The worker's requests go through the RequestServers of its owner's sites.
    TRY(client->request_server_site_bindings().connect(*session));
    client->async_set_site_compatibility_data(Application::the().site_compatibility_data());
    client->async_set_permission_snapshot(session->permission_manager->snapshot_for({}));
    // The User-Agent string, the navigator compatibility mode, and the preferred languages live in this process.
    // Pages get them through their view; a worker gets the same values here, so its navigator never contradicts
    // the page's.
    client->async_set_user_agent(Application::the().user_agent_string());
    client->async_set_navigator_compatibility_mode(Application::the().navigator_compatibility_mode());
    client->async_set_preferred_languages(Application::settings().languages());

    m_clients.append(client);
    return client;
}

void WorkerAgentManager::start_worker(WebWorkerClient& client, Web::HTML::WorkerAgentStartRequest request, CanonicalEnvironmentSettingsObject const& outside_settings, bool has_cross_site_ancestor, bool owner_may_read_local_files, bool has_process_to_itself)
{
    // https://html.spec.whatwg.org/multipage/workers.html#set-up-a-worker-environment-settings-object
    // 3. Let origin be a unique opaque origin if worker global scope's url's scheme is "data"; otherwise outside
    //    settings's origin.
    auto origin = request.url.scheme() == "data"sv ? URL::Origin::create_opaque() : outside_settings.origin();

    // 5. Set settings object's id to a new unique opaque string, [...]
    // 6. If worker global scope is a DedicatedWorkerGlobalScope object, then set settings object's top-level origin to
    //    outside settings's top-level origin.
    // 7. Otherwise, set settings object's top-level origin to an implementation-defined value.
    // NB: As storage partitioning in other browsers does, a shared worker takes the top-level origin of the outside
    //     settings it is created for, too.
    auto inside_settings = make<CanonicalWorkerEnvironmentSettingsObject>(move(origin), outside_settings.top_level_origin(), has_cross_site_ancestor, Web::HTML::EnvironmentId::generate());
    auto environment_id = inside_settings->id();
    auto inside_origin = inside_settings->origin();

    // NB: A worker reads local files as its owner does. A worker of a file origin that local file content starts is
    //     local file content too.
    auto may_read_local_files = inside_origin.is_file_origin() && owner_may_read_local_files;
    client.set_inside_settings(move(inside_settings), may_read_local_files);

    // NB: The worker's URL was parsed with its blob URL entry, so revoking the URL must not stop the worker from
    //     fetching its script.
    if (auto session = client.session())
        client.keep_script_blob_url_until_script_loads(BlobURLHandle::for_url(session->blob_url_store.ptr(), request.url));

    client.async_start_worker(request.url, request.type, request.credentials, request.name, move(request.outside_port), request.outside_settings, request.agent_type, move(inside_origin), move(environment_id), has_process_to_itself);
}

// Sites decide what runs together in a process, as for the documents of CanonicalNavigable::process_to_host(). The worker's
// origin is that of the document starting it, so the process of that document already holds the worker's site.
bool WorkerAgentManager::owner_process_may_host_shared_worker(Owner const& owner, CanonicalEnvironmentSettingsObject const& outside_settings)
{
    if (!owner.client || owner.client->has_requested_close())
        return false;
    auto* page = owner.client->page(owner.page_id);
    if (!page)
        return false;

    // NB: Nothing in a cross-origin isolated process may have been loaded without the consent of its origin. A shared
    //     worker is in an agent cluster of its own, and needs no such consent unless its response says so.
    if (page->hosted_environment_is_cross_origin_isolated(outside_settings.id()).value_or(true))
        return false;

    // NB: Isolating only top-level documents leaves a cross-site frame with the process of its parent's site.
    if (site_isolation_mode() == SiteIsolationMode::TopLevel) {
        auto top_level_origin = outside_settings.top_level_origin();
        return top_level_origin.has_value() && top_level_origin->is_same_site(outside_settings.origin());
    }
    return true;
}

bool WorkerAgentManager::hosts_shared_worker(WebContentClient const& process) const
{
    return any_of(m_agents, [&](auto const& entry) {
        return entry.value.client->host() == &process;
    });
}

void WorkerAgentManager::for_each_client_with_inside_settings(Function<IterationDecision(WebWorkerClient&, CanonicalWorkerEnvironmentSettingsObject const&)> const& callback)
{
    auto clients = m_clients;
    for (auto& client : clients) {
        auto const* inside_settings = client->inside_settings();
        if (inside_settings && callback(*client, *inside_settings) == IterationDecision::Break)
            return;
    }
}

void WorkerAgentManager::update_permission_snapshot(PermissionManager const& manager)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        auto session = client.session();
        if (session && session->permission_manager.ptr() == &manager)
            client.async_set_permission_snapshot(manager.snapshot_for(client.permission_origins()));
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::update_site_compatibility_data(JsonValue const& data)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        client.async_set_site_compatibility_data(data);
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::update_user_agent(String const& user_agent)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        client.async_set_user_agent(user_agent);
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::update_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        client.async_set_navigator_compatibility_mode(mode);
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::update_preferred_languages(Vector<String> const& preferred_languages)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        client.async_set_preferred_languages(preferred_languages);
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::close_worker_agent(WebContentClient& client, Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Owner identity {
        .client = client,
        .token = owner_token,
    };
    remove_owner(agent_id, identity);
}

void WorkerAgentManager::remove_web_content_owner(WebContentClient& client)
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        agent.owners.remove_all_matching([&](Owner const& owner) {
            return owner.client.ptr() == &client;
        });
        forget_removed_outside_settings_owner(agent);
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

void WorkerAgentManager::remove_web_content_page_owner(WebContentClient& client, Web::PageId page_id)
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        auto did_remove_owner = agent.owners.remove_all_matching([&](Owner const& owner) {
            return owner.client.ptr() == &client && owner.page_id == page_id;
        });
        if (!did_remove_owner)
            continue;
        forget_removed_outside_settings_owner(agent);
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

void WorkerAgentManager::remove_private_workers_without_owners()
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto const& entry : m_agents) {
        if (entry.value.client->is_private() == IsPrivate::Yes && entry.value.owners.is_empty())
            agents_to_close.append(entry.key);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
// NB: The processes holding the destinations of step 6 are those hosting an environment whose storage key is
//     sourceStorageKey. Each finds the destinations among its own BroadcastChannel objects.
void WorkerAgentManager::post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage posted_message, CanonicalEnvironmentSettingsObject const& source_settings, IPC::ConnectionBase const& source_connection, IsPrivate is_private)
{
    // 4. Let sourceOrigin be this's relevant settings object's origin.
    // 5. Let sourceStorageKey be the result of running obtain a storage key for non-storage purposes with this's relevant
    //    settings object.
    // NB: The process posting the message names this's relevant settings object.
    auto source_storage_key = obtain_a_storage_key_for_non_storage_purposes(source_settings);
    Web::HTML::BroadcastChannelMessage message {
        .storage_key = source_storage_key,
        .channel_name = move(posted_message.channel_name),
        .source_origin = source_settings.origin(),
        .serialized_message = move(posted_message.serialized_message),
        .shared_store_lifeline = move(posted_message.shared_store_lifeline),
    };

    WebContentClient::for_each_client([&](auto& client) {
        if (&client == &source_connection || client.is_private() != is_private)
            return IterationDecision::Continue;
        if (client.hosts_an_environment_with_storage_key(source_storage_key))
            client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });

    for_each_client_with_inside_settings([&](WebWorkerClient& client, CanonicalWorkerEnvironmentSettingsObject const& inside_settings) {
        if (&client != &source_connection && client.is_private() == is_private && obtain_a_storage_key_for_non_storage_purposes(inside_settings) == source_storage_key)
            client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });
}

void WorkerAgentManager::for_each_request_server_site_bindings(Function<IterationDecision(RequestServerSiteBindings&)> const& callback)
{
    for_each_client_with_inside_settings([&](WebWorkerClient& client, auto const&) {
        return callback(client.request_server_site_bindings());
    });
}

ErrorOr<void> WorkerAgentManager::simulate_request_server_connection_loss_for_testing(WebContentClient& owner, Web::PageId page_id)
{
    auto is_owned_by_page = [&](WorkerAgent const& agent) {
        return any_of(agent.owners, [&](Owner const& candidate) {
            return candidate.client.ptr() == &owner && candidate.page_id == page_id;
        });
    };
    auto runs_a_shared_worker = [&](WebWorkerClient const& client) {
        return any_of(m_agents, [&](auto const& entry) { return entry.value.client.ptr() == &client; });
    };

    Vector<NonnullRefPtr<WebWorkerClient>> clients;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (agent.client->is_open() && is_owned_by_page(agent))
            clients.append(agent.client);
    }
    for (auto& client : m_clients) {
        if (client->is_open() && client->host() == &owner && !runs_a_shared_worker(*client))
            clients.append(client);
    }

    for (auto& client : clients) {
        auto& bindings = client->request_server_site_bindings();
        Vector<Optional<Utf16String>> top_level_sites { OptionalNone {} };
        for (auto& top_level_site : bindings.top_level_sites())
            top_level_sites.append(move(top_level_site));

        for (auto const& top_level_site : top_level_sites) {
            auto request_server_connection = TRY(bindings.replace_client(top_level_site));
            auto response = client->send_sync_but_allow_failure<Messages::WebWorkerServer::SimulateRequestServerConnectionLossAndReconnectForTesting>(move(request_server_connection.handle), request_server_connection.client_id, top_level_site);
            if (!response)
                return Error::from_string_literal("WebWorker disconnected while reconnecting to RequestServer");
        }
    }

    return {};
}

void WorkerAgentManager::notify_worker_script_load_failure(Owner const& owner)
{
    if (owner.client)
        owner.client->async_did_worker_agent_fail_loading_script(owner.token);
}

void WorkerAgentManager::notify_worker_content_security_policy_violation(Owner const& owner, u64 policy_index, String const& effective_directive, URL::URL const& resource)
{
    if (owner.client)
        owner.client->async_did_worker_agent_report_content_security_policy_violation(owner.token, policy_index, effective_directive, resource);
}

void WorkerAgentManager::notify_worker_close(Owner const& owner)
{
    if (owner.client)
        owner.client->async_did_worker_agent_close(owner.token);
}

void WorkerAgentManager::notify_worker_death(Owner const& owner)
{
    if (owner.client)
        owner.client->async_did_worker_agent_die(owner.token);
}

void WorkerAgentManager::worker_did_finish_loading_script(Web::HTML::WorkerAgentId agent_id, bool worker_is_secure_context)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    agent.worker_is_secure_context = worker_is_secure_context;
    agent.outside_settings_owner = {};
}

void WorkerAgentManager::worker_did_fail_loading_script(Web::HTML::WorkerAgentId agent_id)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    if (agent.closing)
        return;

    agent.closing = true;
    agent.outside_settings_owner = {};
    auto owners = agent.owners;
    for (auto const& owner : owners)
        notify_worker_script_load_failure(owner);

    Core::deferred_invoke([this, agent_id] {
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
    });
}

void WorkerAgentManager::worker_did_report_outside_settings_content_security_policy_violation(Web::HTML::WorkerAgentId agent_id, u64 policy_index, String effective_directive, URL::URL resource)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    // NB: Only the worker's script fetch uses outside settings as its client, so its violations are only reported
    //     until that fetch completes, and only to the owner that started the worker, while it still owns it.
    auto const& agent = maybe_agent->value;
    if (agent.closing || !agent.outside_settings_owner.has_value())
        return;
    notify_worker_content_security_policy_violation(*agent.outside_settings_owner, policy_index, effective_directive, resource);
}

void WorkerAgentManager::worker_did_close(Web::HTML::WorkerAgentId agent_id)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    if (agent.closing)
        return;

    agent.closing = true;
    auto owners = agent.owners;
    for (auto const& owner : owners)
        notify_worker_close(owner);

    Core::deferred_invoke([this, agent_id] {
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
    });
}

void WorkerAgentManager::shared_worker_did_close(Web::HTML::WorkerAgentId agent_id)
{
    // NB: The script can run on, and its thread with it, but no constructor may connect to this worker any more.
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;
    auto const& key = maybe_agent->value.shared_worker_key;
    if (m_shared_workers.get(key) == agent_id)
        m_shared_workers.remove(key);
}

void WorkerAgentManager::worker_did_die(Web::HTML::WorkerAgentId agent_id)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    // A close we initiated is expected to drop the connection; any other loss must not look like a clean close.
    auto& agent = maybe_agent->value;
    if (agent.closing) {
        worker_did_close(agent_id);
        return;
    }

    agent.closing = true;
    auto owners = agent.owners;
    for (auto const& owner : owners)
        notify_worker_death(owner);

    Core::deferred_invoke([this, agent_id] {
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
    });
}

void WorkerAgentManager::remove_agent(Web::HTML::WorkerAgentId agent_id, AgentRemovalCause cause)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    // A worker still reachable from an owner is actively needed, and closing it here would be the
    // wrapper-lifetime bug this ownership model exists to prevent.
    if (cause == AgentRemovalCause::OwnerSetEmptied) {
        VERIFY(maybe_agent->value.owners.is_empty());

        // https://html.spec.whatwg.org/multipage/workers.html#in-extended-lifetime-worker
        // A shared worker with an extended lifetime stays protected for the extended lifetime shared worker timeout
        // after its owner set empties, and a new owner may connect to it meanwhile.
        auto& agent = maybe_agent->value;
        if (agent.extended_lifetime) {
            if (!agent.extended_lifetime_timer) {
                agent.extended_lifetime_timer = Core::Timer::create_single_shot(m_extended_lifetime_shared_worker_timeout, [this, agent_id] {
                    remove_agent(agent_id, AgentRemovalCause::ExtendedLifetimeEnded);
                });
                agent.extended_lifetime_timer->start();
            }
            return;
        }
    }

    auto agent = move(maybe_agent->value);
    m_agents.remove(agent_id);

    // NB: The extended lifetime timer may be what is removing the agent, so it outlives its own handler.
    if (agent.extended_lifetime_timer)
        Core::deferred_invoke([timer = move(agent.extended_lifetime_timer)] { });

    // NB: A new worker may have taken this one's place by now.
    if (m_shared_workers.get(agent.shared_worker_key) == agent_id)
        m_shared_workers.remove(agent.shared_worker_key);

    // NB: The owner of a page can be removing the agent from the destructor of the host, which nothing can take a
    //     reference to any more.
    if (agent.client->host()) {
        Core::deferred_invoke([host = agent.client->host()->make_weak_ptr<WebContentClient>()] {
            if (auto process = host.strong_ref())
                process->close_server_if_unused();
        });
    }

    agent.closing = true;
    agent.client->forget_inside_settings_of_closing_worker();

    // NB: A thread leaves as soon as it gets out of the builtin or the request it may be in, and nothing waits for it.
    //     Until it is gone, it counts toward the workers of its top-level site.
    if (agent.client->is_open())
        agent.client->async_close_worker();
}

void WorkerAgentManager::worker_thread_did_exit(WebWorkerClient& client)
{
    Vector<NonnullRefPtr<WebWorkerClient>> exited_clients;
    for (size_t i = m_clients.size(); i > 0; --i) {
        if (m_clients[i - 1].ptr() == &client)
            exited_clients.append(m_clients.take(i - 1));
    }
    // NB: The client is still tearing down its connection, so the last references to it go once it is done.
    Core::deferred_invoke([exited_clients = move(exited_clients)] { });
}

bool WorkerAgentManager::is_same_owner(Owner const& owner, Owner const& identity)
{
    return owner.token == identity.token && owner.client.ptr() == identity.client.ptr();
}

void WorkerAgentManager::forget_removed_outside_settings_owner(WorkerAgent& agent)
{
    if (!agent.outside_settings_owner.has_value())
        return;
    if (!any_of(agent.owners, [&](Owner const& owner) { return is_same_owner(owner, *agent.outside_settings_owner); }))
        agent.outside_settings_owner = {};
}

void WorkerAgentManager::remove_owner(Web::HTML::WorkerAgentId agent_id, Owner const& identity)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    auto agent_owned_by_specified_owner = agent.owners.remove_all_matching([&](Owner const& owner) {
        return is_same_owner(owner, identity);
    });
    forget_removed_outside_settings_owner(agent);

    if (!agent_owned_by_specified_owner)
        return;

    if (agent.owners.is_empty())
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

}
