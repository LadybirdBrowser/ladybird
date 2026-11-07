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
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

// Generous enough for a site that starts a worker per CPU core, as compute-heavy sites do. A worker on a thread costs
// less than a process, but still has a heap, JIT threads and connections to itself, and one page can start them all.
static constexpr size_t maximum_worker_count_per_top_level_site = 64;

WorkerProcessManager& WorkerProcessManager::the()
{
    static auto& manager = *new WorkerProcessManager;
    return manager;
}

WorkerProcessManager::StartedWorkerAgent WorkerProcessManager::start_worker_agent(WebContentClient& owner, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request)
{
    auto abstract_owner = Owner {
        .client = WebContentOwner {
            .client = owner,
            .page_id = page_id,
        },
        .token = request.owner_token,
    };
    return start_worker_agent(move(abstract_owner), outside_settings, move(request), owner.is_private());
}

WorkerProcessManager::StartedWorkerAgent WorkerProcessManager::start_worker_agent(WebWorkerClient& owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request)
{
    auto abstract_owner = Owner {
        .client = WebWorkerOwner {
            .client = owner,
        },
        .token = request.owner_token,
    };
    return start_worker_agent(move(abstract_owner), outside_settings, move(request), owner.is_private());
}

Optional<CanonicalWorkerEnvironmentSettingsObject const&> WorkerProcessManager::inside_settings(Web::HTML::WorkerAgentId agent_id) const
{
    auto agent = m_agents.find(agent_id);
    if (agent == m_agents.end())
        return {};
    return *agent->value.inside_settings;
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
WorkerProcessManager::StartedWorkerAgent WorkerProcessManager::start_worker_agent(Owner owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request, IsPrivate is_private)
{
    // The owner names outside settings, which must be an environment it holds.
    // FIXME: The worker takes the rest of the outside settings, such as their top-level origin, policy container and
    //        cross-origin isolated capability, as the owner's process gave them.
    if (!outside_settings.has_value()) {
        notify_worker_script_load_failure(owner);
        return {};
    }
    request.outside_settings.origin = outside_settings->origin();

    // Only dedicated and shared workers are started on an owner's request.
    if (request.agent_type != Web::HTML::AgentType::DedicatedWorker && request.agent_type != Web::HTML::AgentType::SharedWorker) {
        notify_worker_script_load_failure(owner);
        return {};
    }

    // NB: A worker shares the ancestors of the environment that creates it.
    auto has_cross_site_ancestor = owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            auto* page = web_content_owner.client ? web_content_owner.client->page(web_content_owner.page_id) : nullptr;
            return !page || page->hosted_environment_has_cross_site_ancestor(outside_settings->id()).value_or(true);
        },
        [&](WebWorkerOwner const&) {
            return static_cast<CanonicalWorkerEnvironmentSettingsObject const&>(*outside_settings).has_cross_site_ancestor();
        });

    // 9. Let outsideStorageKey be the result of running obtain a storage key for non-storage purposes given
    //    outsideSettings.
    auto outside_storage_key = obtain_a_storage_key_for_non_storage_purposes(*outside_settings);

    // 11.1. Let workerGlobalScope be null.
    if (request.agent_type == Web::HTML::AgentType::SharedWorker) {
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
                    // AD-HOC: Error firing is routed back over IPC via notify_worker_script_load_failure;
                    //         the spec's queue-global-task happens inside WorkerAgentParent in the
                    //         requesting process.
                    if (agent.worker_type != request.type
                        || agent.credentials != request.credentials
                        || agent.extended_lifetime != request.extended_lifetime
                        || (agent.worker_is_secure_context.has_value() && *agent.worker_is_secure_context != request.caller_is_secure_context)) {
                        notify_worker_script_load_failure(owner);
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
                    auto const* owner_host = owner.client.visit(
                        [](WebContentOwner const& web_content_owner) -> WebContentClient const* {
                            return web_content_owner.client.ptr();
                        },
                        [](WebWorkerOwner const& web_worker_owner) -> WebContentClient const* {
                            return web_worker_owner.client->host();
                        });
                    return { agent.id, {}, owner_host && owner_host == agent.client->host() };
                }
            }

            m_shared_workers.remove(key);
        }
    }

    // NB: A top-level site may run only so many workers at once, wherever its documents and workers start them.
    //     An opaque top-level origin is a site of its own. A worker counts until its thread is gone, and one
    //     whose creator's browsing context has been discarded, leaving it no top-level origin, does not start.
    auto top_level_origin = outside_settings->top_level_origin();
    if (!top_level_origin.has_value()) {
        notify_worker_script_load_failure(owner);
        return {};
    }
    auto counts_toward_top_level_site = [&](IsPrivate worker_is_private, Optional<URL::Origin> const& worker_top_level_origin) {
        return worker_is_private == is_private && worker_top_level_origin.has_value() && worker_top_level_origin->is_same_site(*top_level_origin);
    };
    size_t worker_count_of_top_level_site = 0;
    for (auto const& [id, agent] : m_agents) {
        if (counts_toward_top_level_site(agent.is_private, agent.top_level_origin))
            ++worker_count_of_top_level_site;
    }
    for (auto const& exiting_worker : m_exiting_workers) {
        if (counts_toward_top_level_site(exiting_worker.is_private, exiting_worker.top_level_origin))
            ++worker_count_of_top_level_site;
    }
    if (worker_count_of_top_level_site >= maximum_worker_count_per_top_level_site) {
        notify_worker_script_load_failure(owner);
        return {};
    }

    // 11.6. Otherwise, in parallel, run a worker given worker, urlRecord, outsideSettings, outsidePort,
    //       and options.
    // AD-HOC: For DedicatedWorker there is no shared worker manager step. It always gets a fresh thread in the process
    //         of its owner. A shared worker's thread is in the process of the owner that starts it, if that process may
    //         host it, and otherwise in a process of its own.
    auto agent_id = ++m_next_agent_id;
    RefPtr<WebContentClient> host;
    if (request.agent_type == Web::HTML::AgentType::DedicatedWorker) {
        host = owner.client.visit(
            [](WebContentOwner const& web_content_owner) -> RefPtr<WebContentClient> {
                return web_content_owner.client.strong_ref();
            },
            [](WebWorkerOwner const& web_worker_owner) -> RefPtr<WebContentClient> {
                return web_worker_owner.client->host();
            });
    } else if (auto const* web_content_owner = owner.client.get_pointer<WebContentOwner>(); web_content_owner && owner_process_may_host_shared_worker(*web_content_owner, *outside_settings)) {
        host = web_content_owner->client.strong_ref();
    }
    // NB: Nothing would close a process started for a dedicated worker, as only shared workers keep a process alive.
    if (request.agent_type == Web::HTML::AgentType::DedicatedWorker && !host) {
        notify_worker_script_load_failure(owner);
        return {};
    }
    bool const host_has_only_this_worker = !host;
    Optional<IPC::TransportHandle> thread_transport;
    auto client_or_error = [&]() -> ErrorOr<NonnullRefPtr<WebWorkerClient>> {
        auto transport = TRY(IPC::Transport::create_paired());
        if (host_has_only_this_worker) {
            host = TRY(Application::the().launch_web_content_process_without_page(is_private));
            host->async_start_worker_thread(move(transport.remote_handle), request.agent_type);
        } else {
            thread_transport = move(transport.remote_handle);
        }
        auto client = TRY(try_make_ref_counted<WebWorkerClient>(move(transport.local), is_private, agent_id));
        client->set_host(*host);

        // The worker's RequestServer client uses the cookies of the session the worker itself belongs to.
        auto session = client->session();
        if (!session)
            session = Application::session_for_new_view(is_private);
        // The worker makes its requests for the sites its owner makes them for.
        owner.client.visit(
            [&](WebContentOwner const& web_content_owner) {
                if (web_content_owner.client)
                    client->request_server_site_bindings().bind_sites_of(web_content_owner.client->request_server_site_bindings());
            },
            [&](WebWorkerOwner const& web_worker_owner) {
                client->request_server_site_bindings().bind_sites_of(web_worker_owner.client->request_server_site_bindings());
            });

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

        if (auto compositor_handle = Application::the().connect_new_compositor_canvas_client(); !compositor_handle.is_error())
            client->async_connect_to_compositor(compositor_handle.release_value());
        return client;
    }();
    // A worker the system cannot give a thread fails to load, as one whose script cannot be fetched does.
    if (client_or_error.is_error()) {
        dbgln("Unable to start a worker: {}", client_or_error.error());
        if (host_has_only_this_worker && host)
            host->close_server_if_unused();
        notify_worker_script_load_failure(owner);
        return {};
    }
    auto client = client_or_error.release_value();

    Vector<Owner> owners;
    owners.append(owner);

    // https://html.spec.whatwg.org/multipage/workers.html#set-up-a-worker-environment-settings-object
    // 3. Let origin be a unique opaque origin if worker global scope's url's scheme is "data"; otherwise outside
    //    settings's origin.
    auto origin = request.url.scheme() == "data"sv ? URL::Origin::create_opaque() : outside_settings->origin();

    // 5. Set settings object's id to a new unique opaque string, [...]
    // 6. If worker global scope is a DedicatedWorkerGlobalScope object, then set settings object's top-level origin to
    //    outside settings's top-level origin.
    // 7. Otherwise, set settings object's top-level origin to an implementation-defined value.
    // NB: As storage partitioning in other browsers does, a shared worker takes the top-level origin of the outside
    //     settings it is created for, too.
    auto inside_settings = make<CanonicalWorkerEnvironmentSettingsObject>(move(origin), outside_settings->top_level_origin(), has_cross_site_ancestor, Web::HTML::EnvironmentId::generate());
    auto environment_id = inside_settings->id();

    // NB: The task manager lists a process with the site of its pages. One that has none left only runs on for its shared
    //     workers, so it goes by the site of their origin.
    if (auto process = Application::process_manager().find_child(client->child_id()); request.agent_type == Web::HTML::AgentType::SharedWorker && process.has_value()) {
        if (auto const& worker_origin = inside_settings->origin(); !worker_origin.is_opaque() && !worker_origin.is_file_origin())
            process->set_site_url(URL::Parser::basic_parse(worker_origin.serialize()));
    }

    // NB: A worker reads local files as its owner does. A worker of a file origin that local file content starts is
    //     local file content too.
    auto may_read_local_files = inside_settings->origin().is_file_origin()
        && owner.client.visit(
            [](WebContentOwner const& web_content_owner) {
                return web_content_owner.client && web_content_owner.client->may_read_local_files();
            },
            [&](WebWorkerOwner const& web_worker_owner) {
                for (auto const& [id, owner_agent] : m_agents) {
                    if (owner_agent.client.ptr() == web_worker_owner.client.ptr())
                        return owner_agent.may_read_local_files;
                }
                return false;
            });

    // AD-HOC: Seed worker_is_secure_context with the caller's value so reuse requests arriving before
    //         the worker finishes loading still get a mismatch check.
    //         worker_did_finish_loading_script overwrites this with the worker's actual value (which
    //         inherits from outside settings, so should match).
    WorkerAgent agent {
        .id = agent_id,
        .client = client,
        .agent_type = request.agent_type,
        .worker_type = request.type,
        .credentials = request.credentials,
        .extended_lifetime = request.extended_lifetime,
        .extended_lifetime_timer = {},
        .worker_is_secure_context = request.caller_is_secure_context,
        .is_private = is_private,
        .top_level_origin = top_level_origin,
        .shared_worker_key = {},
        .owners = move(owners),
        .outside_settings_owner = owner,
        .inside_settings = move(inside_settings),
        .may_read_local_files = may_read_local_files,
        .script_blob_url = {},
    };

    // NB: The worker's URL was parsed with its blob URL entry, so revoking the URL must not stop the worker from
    //     fetching its script.
    if (auto session = client->session())
        agent.script_blob_url = BlobURLHandle::for_url(session->blob_url_store.ptr(), request.url);

    if (request.agent_type == Web::HTML::AgentType::SharedWorker) {
        agent.shared_worker_key = SharedWorkerKey {
            .is_private = is_private,
            .storage_key = move(outside_storage_key),
            .url = request.url,
            .name = Utf16String::from_utf8(request.name),
            .top_level_site = network_isolation_top_level_site(*outside_settings),
            .has_cross_site_ancestor = has_cross_site_ancestor,
        };
        m_shared_workers.set(*agent.shared_worker_key, agent_id);
    }

    auto inside_origin = agent.inside_settings->origin();
    m_agents.set(agent_id, move(agent));
    client->async_start_worker(request.url, request.type, request.credentials, request.name, move(request.outside_port), request.outside_settings, request.agent_type, move(inside_origin), move(environment_id), host_has_only_this_worker);

    return { agent_id, move(thread_transport), !host_has_only_this_worker };
}

// Sites decide what runs together in a process, as for the documents of CanonicalNavigable::process_to_host(). The worker's
// origin is that of the document starting it, so the process of that document already holds the worker's site.
bool WorkerProcessManager::owner_process_may_host_shared_worker(WebContentOwner const& owner, CanonicalEnvironmentSettingsObject const& outside_settings)
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

bool WorkerProcessManager::hosts_shared_worker(WebContentClient const& process) const
{
    return any_of(m_agents, [&](auto const& entry) {
        return entry.value.agent_type == Web::HTML::AgentType::SharedWorker && entry.value.client->host() == &process;
    });
}

void WorkerProcessManager::update_permission_snapshot(PermissionManager const& manager)
{
    for (auto const& agent : m_agents) {
        auto session = agent.value.client->session();
        if (session && session->permission_manager.ptr() == &manager)
            agent.value.client->async_set_permission_snapshot(manager.snapshot_for(agent.value.client->permission_origins()));
    }
}

Optional<URL::Origin> WorkerProcessManager::top_level_origin(Web::HTML::WorkerAgentId agent_id) const
{
    auto agent = m_agents.find(agent_id);
    if (agent == m_agents.end())
        return {};
    return agent->value.top_level_origin;
}

void WorkerProcessManager::update_site_compatibility_data(JsonValue const& data)
{
    for (auto const& agent : m_agents)
        agent.value.client->async_set_site_compatibility_data(data);
}

void WorkerProcessManager::update_user_agent(String const& user_agent)
{
    for (auto const& agent : m_agents)
        agent.value.client->async_set_user_agent(user_agent);
}

void WorkerProcessManager::update_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    for (auto const& agent : m_agents)
        agent.value.client->async_set_navigator_compatibility_mode(mode);
}

void WorkerProcessManager::update_preferred_languages(Vector<String> const& preferred_languages)
{
    for (auto const& agent : m_agents)
        agent.value.client->async_set_preferred_languages(preferred_languages);
}

void WorkerProcessManager::close_worker_agent(WebContentClient& client, Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Owner identity {
        .client = WebContentOwner { .client = client },
        .token = owner_token,
    };
    remove_owner(agent_id, identity);
}

void WorkerProcessManager::close_worker_agent(WebWorkerClient& client, Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Owner identity {
        .client = WebWorkerOwner { .client = client },
        .token = owner_token,
    };
    remove_owner(agent_id, identity);
}

void WorkerProcessManager::remove_web_content_owner(WebContentClient& client)
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        agent.owners.remove_all_matching([&](Owner const& owner) {
            auto const* web_content_owner = owner.client.get_pointer<WebContentOwner>();
            return web_content_owner && web_content_owner->client.ptr() == &client;
        });
        forget_removed_outside_settings_owner(agent);
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

void WorkerProcessManager::remove_web_content_page_owner(WebContentClient& client, Web::PageId page_id)
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        auto did_remove_owner = agent.owners.remove_all_matching([&](Owner const& owner) {
            auto const* web_content_owner = owner.client.get_pointer<WebContentOwner>();
            return web_content_owner && web_content_owner->client.ptr() == &client && web_content_owner->page_id == page_id;
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

void WorkerProcessManager::remove_private_workers_without_owners()
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto const& entry : m_agents) {
        if (entry.value.is_private == IsPrivate::Yes && entry.value.owners.is_empty())
            agents_to_close.append(entry.key);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
}

void WorkerProcessManager::remove_web_worker_owner(WebWorkerClient& client)
{
    Vector<Web::HTML::WorkerAgentId> agents_to_close;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        agent.owners.remove_all_matching([&](Owner const& owner) {
            auto const* web_worker_owner = owner.client.get_pointer<WebWorkerOwner>();
            return web_worker_owner && web_worker_owner->client.ptr() == &client;
        });
        forget_removed_outside_settings_owner(agent);
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
// NB: The processes holding the destinations of step 6 are those hosting an environment whose storage key is
//     sourceStorageKey. Each finds the destinations among its own BroadcastChannel objects.
void WorkerProcessManager::post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage posted_message, CanonicalEnvironmentSettingsObject const& source_settings, IPC::ConnectionBase const& source_connection, IsPrivate is_private)
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

    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (agent.client.ptr() == &source_connection || agent.is_private != is_private)
            continue;
        if (obtain_a_storage_key_for_non_storage_purposes(*agent.inside_settings) == source_storage_key)
            agent.client->async_broadcast_channel_message(message);
    }
}

void WorkerProcessManager::for_each_request_server_site_bindings(Function<IterationDecision(RequestServerSiteBindings&)> const& callback)
{
    for (auto& entry : m_agents) {
        if (callback(entry.value.client->request_server_site_bindings()) == IterationDecision::Break)
            return;
    }
}

ErrorOr<void> WorkerProcessManager::simulate_request_server_connection_loss_for_testing(WebContentClient& owner, Web::PageId page_id)
{
    auto is_owned_by_page = [&](WorkerAgent const& agent) {
        return any_of(agent.owners, [&](Owner const& candidate) {
            auto const* web_content_owner = candidate.client.get_pointer<WebContentOwner>();
            return web_content_owner && web_content_owner->client.ptr() == &owner && web_content_owner->page_id == page_id;
        });
    };

    Vector<NonnullRefPtr<WebWorkerClient>> clients;
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (agent.client->is_open() && is_owned_by_page(agent))
            clients.append(agent.client);
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

void WorkerProcessManager::notify_worker_script_load_failure(Owner const& owner)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_fail_loading_script(owner.token);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_fail_loading_script(owner.token);
        });
}

void WorkerProcessManager::notify_worker_exception(Owner const& owner, Utf16String const& message, Utf16String const& filename, u32 lineno, u32 colno)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_report_exception(owner.token, message, filename, lineno, colno);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_report_exception(owner.token, message, filename, lineno, colno);
        });
}

void WorkerProcessManager::notify_worker_content_security_policy_violation(Owner const& owner, u64 policy_index, String const& effective_directive, URL::URL const& resource)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_report_content_security_policy_violation(owner.token, policy_index, effective_directive, resource);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_report_content_security_policy_violation(owner.token, policy_index, effective_directive, resource);
        });
}

WorkerProcessManager::WorkerAgent* WorkerProcessManager::agent_owned_by(WebContentClient& client, Web::HTML::WorkerAgentId agent_id)
{
    auto agent = m_agents.find(agent_id);
    if (agent == m_agents.end())
        return nullptr;
    for (auto const& owner : agent->value.owners) {
        auto const* web_content_owner = owner.client.get_pointer<WebContentOwner>();
        if (web_content_owner && web_content_owner->client.ptr() == &client)
            return &agent->value;
    }
    return nullptr;
}

void WorkerProcessManager::rtc_transform_init(WebContentClient& client, Web::HTML::WorkerAgentId agent_id, u64 transform_id, Web::HTML::SerializedTransferRecord options_record)
{
    if (auto* agent = agent_owned_by(client, agent_id))
        agent->client->async_rtc_transform_init(transform_id, move(options_record));
}

void WorkerProcessManager::rtc_transform_teardown(WebContentClient& client, Web::HTML::WorkerAgentId agent_id, u64 transform_id)
{
    if (auto* agent = agent_owned_by(client, agent_id))
        agent->client->async_rtc_transform_teardown(transform_id);
}

void WorkerProcessManager::rtc_transform_encoded_audio_frame(WebContentClient& client, Web::HTML::WorkerAgentId agent_id, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    if (auto* agent = agent_owned_by(client, agent_id))
        agent->client->async_rtc_transform_encoded_audio_frame(transform_id, payload.bytes(), ssrc, payload_type, rtp_timestamp, sequence_number);
}

void WorkerProcessManager::worker_did_write_rtc_transform_encoded_audio_frame(Web::HTML::WorkerAgentId agent_id, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    auto agent = m_agents.find(agent_id);
    if (agent == m_agents.end())
        return;

    for (auto const& owner : agent->value.owners) {
        owner.client.visit(
            [&](WebContentOwner const& web_content_owner) {
                if (web_content_owner.client)
                    web_content_owner.client->async_did_worker_rtc_transform_encoded_audio_frame_written(owner.token, transform_id, payload, ssrc, payload_type, rtp_timestamp, sequence_number);
            },
            [&](WebWorkerOwner const&) {
                // FIXME: Route transform frames back to nested-worker owners once the WebWorkerServer endpoint carries this message.
            });
    }
}

void WorkerProcessManager::notify_worker_close(Owner const& owner)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_close(owner.token);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_close(owner.token);
        });
}

void WorkerProcessManager::notify_worker_death(Owner const& owner)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_die(owner.token);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_die(owner.token);
        });
}

void WorkerProcessManager::worker_did_finish_loading_script(Web::HTML::WorkerAgentId agent_id, bool worker_is_secure_context)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    agent.worker_is_secure_context = worker_is_secure_context;
    agent.script_blob_url = {};
    agent.outside_settings_owner = {};
}

void WorkerProcessManager::worker_did_fail_loading_script(Web::HTML::WorkerAgentId agent_id)
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

void WorkerProcessManager::worker_did_report_exception(Web::HTML::WorkerAgentId agent_id, Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    for (auto const& owner : maybe_agent->value.owners)
        notify_worker_exception(owner, message, filename, lineno, colno);
}

void WorkerProcessManager::worker_did_report_outside_settings_content_security_policy_violation(Web::HTML::WorkerAgentId agent_id, u64 policy_index, String effective_directive, URL::URL resource)
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

void WorkerProcessManager::worker_did_close(Web::HTML::WorkerAgentId agent_id)
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

void WorkerProcessManager::worker_did_die(Web::HTML::WorkerAgentId agent_id)
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

void WorkerProcessManager::worker_did_request_file(Web::HTML::WorkerAgentId agent_id, ByteString path, i32 request_id)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    // NB: As a page, a worker reads a local file only through this broker, and only local file content may.
    auto& client = maybe_agent->value.client;
    if (!maybe_agent->value.may_read_local_files) {
        client->async_handle_file_return(EACCES, {}, request_id);
        return;
    }

    auto file = open_local_file_for_renderer(path);
    if (file.is_error())
        client->async_handle_file_return(file.error().code(), {}, request_id);
    else
        client->async_handle_file_return(0, file.release_value(), request_id);
}

void WorkerProcessManager::remove_agent(Web::HTML::WorkerAgentId agent_id, AgentRemovalCause cause)
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

    if (agent.shared_worker_key.has_value())
        m_shared_workers.remove(*agent.shared_worker_key);

    // NB: The owner of a page can be removing the agent from the destructor of the host, which nothing can take a
    //     reference to any more.
    if (agent.agent_type == Web::HTML::AgentType::SharedWorker && agent.client->host()) {
        Core::deferred_invoke([host = agent.client->host()->make_weak_ptr<WebContentClient>()] {
            if (auto process = host.strong_ref())
                process->close_server_if_unused();
        });
    }

    agent.closing = true;
    if (!agent.client->is_open())
        return;
    agent.client->async_close_worker();

    // NB: A thread leaves as soon as it gets out of the builtin or the request it may be in, and nothing waits for it.
    //     Until it is gone, it counts toward the workers of its top-level site.
    m_exiting_workers.append({ agent.client, agent.is_private, move(agent.top_level_origin) });
}

void WorkerProcessManager::worker_thread_did_exit(WebWorkerClient& client)
{
    Vector<ExitingWorker> exited_workers;
    for (size_t i = m_exiting_workers.size(); i > 0; --i) {
        if (m_exiting_workers[i - 1].client.ptr() == &client)
            exited_workers.append(m_exiting_workers.take(i - 1));
    }
    // NB: The client is still tearing down its connection, so the last references to it go once it is done.
    Core::deferred_invoke([exited_workers = move(exited_workers)] { });
}

bool WorkerProcessManager::is_same_owner(Owner const& owner, Owner const& identity)
{
    if (owner.token != identity.token)
        return false;

    if (auto const* incoming = identity.client.get_pointer<WebContentOwner>()) {
        auto const* candidate = owner.client.get_pointer<WebContentOwner>();
        return candidate && candidate->client.ptr() == incoming->client.ptr();
    }
    if (auto const* incoming = identity.client.get_pointer<WebWorkerOwner>()) {
        auto const* candidate = owner.client.get_pointer<WebWorkerOwner>();
        return candidate && candidate->client.ptr() == incoming->client.ptr();
    }
    return false;
}

void WorkerProcessManager::forget_removed_outside_settings_owner(WorkerAgent& agent)
{
    if (!agent.outside_settings_owner.has_value())
        return;
    if (!any_of(agent.owners, [&](Owner const& owner) { return is_same_owner(owner, *agent.outside_settings_owner); }))
        agent.outside_settings_owner = {};
}

void WorkerProcessManager::remove_owner(Web::HTML::WorkerAgentId agent_id, Owner const& identity)
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
    else if (agent.agent_type == Web::HTML::AgentType::DedicatedWorker)
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
}

}
