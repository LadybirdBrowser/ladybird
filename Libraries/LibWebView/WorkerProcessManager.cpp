/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibIPC/File.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

WorkerProcessManager& WorkerProcessManager::the()
{
    static auto& manager = *new WorkerProcessManager;
    return manager;
}

Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(WebContentClient& owner, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request)
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

Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(WebWorkerClient& owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request)
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
Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(Owner owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest request, IsPrivate is_private)
{
    // The owner names outside settings, which must be an environment it holds.
    // FIXME: The worker takes the rest of the outside settings, such as their top-level origin, policy container and
    //        cross-origin isolated capability, as the owner's process gave them.
    if (!outside_settings.has_value()) {
        notify_worker_script_load_failure(owner);
        return 0;
    }
    request.outside_settings.origin = outside_settings->origin();

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
                    // FIXME: The extendedLifetime comparison becomes observable once SharedWorkerOptions is implemented.
                    if (agent.worker_type != request.type
                        || agent.credentials != request.credentials
                        || agent.extended_lifetime != request.extended_lifetime
                        || (agent.worker_is_secure_context.has_value() && *agent.worker_is_secure_context != request.caller_is_secure_context)) {
                        notify_worker_script_load_failure(owner);
                        return 0;
                    }

                    // 11.5.8. Append the relevant owner to add given outsideSettings to
                    //         workerGlobalScope's owner set.
                    // AD-HOC: The browser-side mirror lives in `agent.owners`; the worker-process
                    //         owner_set() is appended inside connect_shared_worker_impl (which also
                    //         handles steps 11.5.5-11.5.7).
                    agent.owners.append(owner);
                    agent.client->async_connect_shared_worker(move(request.outside_port), request.outside_settings);
                    return agent.id;
                }
            }

            m_shared_workers.remove(key);
        }
    }

    // 11.6. Otherwise, in parallel, run a worker given worker, urlRecord, outsideSettings, outsidePort,
    //       and options.
    // AD-HOC: For DedicatedWorker there is no shared worker manager step; we always launch a fresh
    //         worker process here.
    auto agent_id = ++m_next_agent_id;
    auto client = MUST(launch_web_worker_process(request.agent_type, is_private, agent_id));

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

    auto request_server_connection = MUST(connect_new_request_server_client(*session, RequestServer::SiteBinding::Bound));
    auto image_decoder = MUST(launch_image_decoder_process());
#if defined(HAVE_WASM_COMPILER_SERVICE)
    auto wasm_compiler_handle = MUST(connect_new_wasm_compiler_client());
#endif

    client->request_server_site_bindings().did_connect(request_server_connection.client_id);
    client->async_connect_to_request_server(move(request_server_connection.handle));
    client->async_set_site_compatibility_data(Application::the().site_compatibility_data());
    connect_to_image_decoder(*client, move(image_decoder));
#if defined(HAVE_WASM_COMPILER_SERVICE)
    client->async_connect_to_wasm_compiler(wasm_compiler_handle);
#endif

    if (auto compositor_handle = Application::the().connect_new_compositor_canvas_client(); !compositor_handle.is_error())
        client->async_connect_to_compositor(compositor_handle.release_value());

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
        .worker_is_secure_context = request.caller_is_secure_context,
        .is_private = is_private,
        .shared_worker_key = {},
        .owners = move(owners),
        .inside_settings = move(inside_settings),
    };

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
    client->async_start_worker(request.url, request.type, request.credentials, request.name, move(request.outside_port), request.outside_settings, request.agent_type, move(inside_origin), move(environment_id));

    return agent_id;
}

void WorkerProcessManager::update_site_compatibility_data(JsonValue const& data)
{
    for (auto const& agent : m_agents)
        agent.value.client->async_set_site_compatibility_data(data);
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
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
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
        if (agent.owners.is_empty())
            agents_to_close.append(agent.id);
    }

    for (auto agent_id : agents_to_close)
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
// NB: The processes holding the destinations of step 6 are those hosting an environment whose storage key is
//     sourceStorageKey. Each finds the destinations among its own BroadcastChannel objects.
void WorkerProcessManager::post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage posted_message, CanonicalEnvironmentSettingsObject const& source_settings, pid_t source_process_id, IsPrivate is_private)
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
        .shared_buffers = move(posted_message.shared_buffers),
        .source_process_id = source_process_id,
        .source_channel_id = posted_message.source_channel_id,
    };

    WebContentClient::for_each_client([&](auto& client) {
        if (client.pid() == source_process_id || client.is_private() != is_private)
            return IterationDecision::Continue;
        if (client.hosts_an_environment_with_storage_key(source_storage_key))
            client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });

    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (agent.client->pid() == source_process_id || agent.is_private != is_private)
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

ErrorOr<void> WorkerProcessManager::reconnect_to_request_server()
{
    return reconnect_to_request_server([](auto const&) { return true; });
}

ErrorOr<void> WorkerProcessManager::reconnect_to_request_server(Function<bool(WorkerAgent const&)> should_reconnect)
{
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (!agent.client->is_open() || !should_reconnect(agent))
            continue;

        // A worker whose session is gone has nothing left to fetch for.
        auto session = agent.client->session();
        if (!session)
            continue;

        auto request_server_connection = TRY(connect_new_request_server_client(*session, RequestServer::SiteBinding::Bound));
        agent.client->request_server_site_bindings().did_connect(request_server_connection.client_id);
        agent.client->async_connect_to_request_server(move(request_server_connection.handle));
    }
    return {};
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
        auto session = client->session();
        VERIFY(session);
        auto request_server_connection = TRY(connect_new_request_server_client(*session, RequestServer::SiteBinding::Bound));
        client->request_server_site_bindings().did_connect(request_server_connection.client_id);
        auto response = client->send_sync_but_allow_failure<Messages::WebWorkerServer::SimulateRequestServerConnectionLossAndReconnectForTesting>(move(request_server_connection.handle));
        if (!response)
            return Error::from_string_literal("WebWorker disconnected while reconnecting to RequestServer");
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

    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        maybe_agent->value.client->async_handle_file_return(file.error().code(), {}, request_id);
    else
        maybe_agent->value.client->async_handle_file_return(0, IPC::File::adopt_file(file.release_value()), request_id);
}

void WorkerProcessManager::remove_agent(Web::HTML::WorkerAgentId agent_id, AgentRemovalCause cause)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    // A worker still reachable from an owner is actively needed, and closing it here would be the
    // wrapper-lifetime bug this ownership model exists to prevent.
    if (cause == AgentRemovalCause::OwnerSetEmptied)
        VERIFY(maybe_agent->value.owners.is_empty());

    auto agent = move(maybe_agent->value);
    m_agents.remove(agent_id);

    if (agent.shared_worker_key.has_value())
        m_shared_workers.remove(*agent.shared_worker_key);

    agent.closing = true;
    if (agent.client->is_open())
        agent.client->async_close_worker();
}

void WorkerProcessManager::remove_owner(Web::HTML::WorkerAgentId agent_id, Owner const& identity)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    auto agent_owned_by_specified_owner = agent.owners.remove_all_matching([&](Owner const& owner) {
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
    });

    if (!agent_owned_by_specified_owner)
        return;

    if (agent.owners.is_empty())
        remove_agent(agent_id, AgentRemovalCause::OwnerSetEmptied);
    else if (agent.agent_type == Web::HTML::AgentType::DedicatedWorker)
        remove_agent(agent_id, AgentRemovalCause::AgentTerminated);
}

Optional<u64> WorkerProcessManager::exclusive_performance_owner(pid_t pid) const
{
    HashTable<Web::HTML::WorkerAgentId> visiting;
    Function<Optional<u64>(WorkerAgent const&)> resolve = [&](WorkerAgent const& agent) -> Optional<u64> {
        if (agent.closing || visiting.contains(agent.id))
            return {};
        visiting.set(agent.id);
        ScopeGuard remove_visit = [&] { visiting.remove(agent.id); };
        Optional<u64> result;
        for (auto const& owner : agent.owners) {
            auto id = owner.client.visit(
                [&](WebContentOwner const& content) -> Optional<u64> {
                if (!content.client)
                    return {};
                auto* page = content.client->page(content.page_id);
                if (!page)
                    return {};
                return page->view().view_id(); },
                [&](WebWorkerOwner const& worker) -> Optional<u64> {
                for (auto const& candidate : m_agents) {
                    if (candidate.value.client.ptr() == worker.client.ptr())
                        return resolve(candidate.value);
                }
                return {}; });
            if (!id.has_value() || (result.has_value() && *result != *id))
                return {};
            result = id;
        }
        return result;
    };
    for (auto const& agent : m_agents) {
        if (agent.value.client->pid() == pid)
            return resolve(agent.value);
    }
    return {};
}

}
