/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibIPC/File.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

WorkerProcessManager& WorkerProcessManager::the()
{
    static auto& manager = *new WorkerProcessManager;
    return manager;
}

Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(WebContentClient& owner, u64 page_id, Web::HTML::WorkerAgentStartRequest request)
{
    auto abstract_owner = Owner {
        .client = WebContentOwner {
            .client = owner,
            .page_id = page_id,
        },
        .token = request.owner_token,
    };
    return start_worker_agent(move(abstract_owner), move(request));
}

Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(WebWorkerClient& owner, Web::HTML::WorkerAgentStartRequest request)
{
    auto abstract_owner = Owner {
        .client = WebWorkerOwner {
            .client = owner,
        },
        .token = request.owner_token,
    };
    return start_worker_agent(move(abstract_owner), move(request));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker
Web::HTML::WorkerAgentId WorkerProcessManager::start_worker_agent(Owner owner, Web::HTML::WorkerAgentStartRequest request)
{
    // 11.1. Let workerGlobalScope be null.
    if (request.agent_type == Web::Bindings::AgentType::SharedWorker) {
        SharedWorkerKey key {
            .storage_key = request.storage_key,
            .url = request.url,
            .name = request.name,
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
                    notify_worker_script_load_success(owner);
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
    auto client = MUST(launch_web_worker_process(request.agent_type, agent_id));

    auto request_server_handle = MUST(connect_new_request_server_client());
    auto image_decoder_handle = MUST(connect_new_image_decoder_client());
    client->async_connect_to_request_server(move(request_server_handle));
    client->async_connect_to_image_decoder(move(image_decoder_handle));

    Vector<Owner> owners;
    owners.append(owner);

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
        .shared_worker_key = {},
        .owners = move(owners),
    };

    if (request.agent_type == Web::Bindings::AgentType::SharedWorker) {
        agent.shared_worker_key = SharedWorkerKey {
            .storage_key = request.storage_key,
            .url = request.url,
            .name = request.name,
        };
        m_shared_workers.set(*agent.shared_worker_key, agent_id);
    }

    m_agents.set(agent_id, move(agent));
    client->async_start_worker(request.url, request.type, request.credentials, request.name, move(request.outside_port), request.outside_settings, request.agent_type);

    return agent_id;
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
        remove_agent(agent_id);
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
        remove_agent(agent_id);
}

void WorkerProcessManager::broadcast_channel_message_from_web_content(Web::HTML::BroadcastChannelMessage const& message)
{
    for (auto& entry : m_agents) {
        auto& agent = entry.value;
        if (agent.client->pid() == message.source_process_id)
            continue;
        agent.client->async_broadcast_channel_message(message);
    }
}

void WorkerProcessManager::notify_worker_script_load_success(Owner const& owner)
{
    owner.client.visit(
        [&](WebContentOwner const& web_content_owner) {
            if (web_content_owner.client)
                web_content_owner.client->async_did_worker_agent_finish_loading_script(owner.token);
        },
        [&](WebWorkerOwner const& web_worker_owner) {
            web_worker_owner.client->async_did_worker_agent_finish_loading_script(owner.token);
        });
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

void WorkerProcessManager::notify_worker_exception(Owner const& owner, String const& message, String const& filename, u32 lineno, u32 colno)
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

void WorkerProcessManager::worker_did_finish_loading_script(Web::HTML::WorkerAgentId agent_id, bool worker_is_secure_context)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

    auto& agent = maybe_agent->value;
    agent.worker_is_secure_context = worker_is_secure_context;

    for (auto const& owner : agent.owners)
        notify_worker_script_load_success(owner);
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
        remove_agent(agent_id);
    });
}

void WorkerProcessManager::worker_did_report_exception(Web::HTML::WorkerAgentId agent_id, String message, String filename, u32 lineno, u32 colno)
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
        remove_agent(agent_id);
    });
}

void WorkerProcessManager::worker_did_die(Web::HTML::WorkerAgentId agent_id)
{
    worker_did_close(agent_id);
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

void WorkerProcessManager::worker_did_post_broadcast_channel_message(Web::HTML::WorkerAgentId agent_id, Web::HTML::BroadcastChannelMessage message)
{
    WebContentClient::for_each_client([&](auto& client) {
        if (client.pid() == message.source_process_id)
            return IterationDecision::Continue;
        client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });

    for (auto& entry : m_agents) {
        if (entry.key == agent_id)
            continue;
        auto& agent = entry.value;
        if (agent.client->pid() == message.source_process_id)
            continue;
        agent.client->async_broadcast_channel_message(message);
    }
}

void WorkerProcessManager::remove_agent(Web::HTML::WorkerAgentId agent_id)
{
    auto maybe_agent = m_agents.find(agent_id);
    if (maybe_agent == m_agents.end())
        return;

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

    if (agent.agent_type == Web::Bindings::AgentType::DedicatedWorker || agent.owners.is_empty())
        remove_agent(agent_id);
}

}
