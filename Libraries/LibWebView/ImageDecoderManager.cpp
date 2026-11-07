/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/ImageDecoderControlClient.h>
#include <LibWebView/ImageDecoderManager.h>

namespace WebView {

ImageDecoderManager& ImageDecoderManager::the()
{
    return Application::image_decoder_manager();
}

ImageDecoderManager::ImageDecoderManager() = default;
ImageDecoderManager::~ImageDecoderManager() = default;

ErrorOr<IPC::TransportHandle> ImageDecoderManager::connect_new_client(u64 client_child_id, RefPtr<BrowsingSession> const& session, Optional<Utf16String> site)
{
    if (site_isolation_mode() != SiteIsolationMode::IFrame || !session)
        site.clear();

    // NB: A decoder that died is of no use to anyone, whether or not its clients have noticed yet.
    m_instances.remove_all_matching([&](auto const& instance) {
        if (instance.control_client->is_open())
            return false;
        did_lose_decoder(instance);
        return true;
    });

    auto instance = m_instances.first_matching([&](auto const& instance) {
        if (site.has_value())
            return instance.site == site && instance.session.ptr() == session.ptr();
        return !instance.site.has_value() && instance.client_counts.contains(client_child_id);
    });
    auto channel = TRY(IPC::TransportHandle::create_pair());
    if (!instance.has_value()) {
        auto control_client = TRY(launch_image_decoder_process(client_child_id, site));
        m_instances.append({ .control_client = move(control_client), .session = session, .site = move(site), .client_counts = {} });
        instance = m_instances.last();
    }

    instance->control_client->async_connect_new_client(channel.first);
    ++instance->client_counts.ensure(client_child_id, [] { return 0uz; });
    return move(channel.second);
}

Optional<IPC::TransportHandle> ImageDecoderManager::connect_requesting_process(u64 client_child_id, RefPtr<BrowsingSession> const& session, Optional<Utf16String> site, Optional<u64>& client_generation)
{
    // NB: A request can still be queued when the process's exit is handled. Connecting it then would leave a client that
    //     never exits, which would keep its decoder alive for as long as the Browser runs.
    if (!Application::process_manager().find_child(client_child_id).has_value())
        return {};

    auto handle = connect_new_client(client_child_id, session, move(site));
    if (handle.is_error()) {
        warnln("Unable to connect an ImageDecoder client: {}", handle.error());
        return {};
    }
    note_connected_client(client_child_id, client_generation);
    return handle.release_value();
}

void ImageDecoderManager::note_connected_client(u64 client_child_id, Optional<u64>& client_generation)
{
    auto generation_of_lost_client = exchange(client_generation, this->client_generation(client_child_id));
    if (generation_of_lost_client.has_value())
        process_did_release_decoder(client_child_id, *generation_of_lost_client);
}

void ImageDecoderManager::did_lose_decoder(Instance const& instance)
{
    for (auto const& [client_child_id, count] : instance.client_counts)
        ++m_client_generations.ensure(client_child_id, [] { return 0uz; });
}

bool ImageDecoderManager::remove_client_from_decoder_of_site(u64 client_child_id)
{
    bool was_removed = false;
    m_instances.remove_all_matching([&](auto& instance) {
        if (!instance.site.has_value() || !instance.client_counts.remove(client_child_id))
            return false;
        was_removed = true;
        return instance.client_counts.is_empty();
    });
    if (was_removed)
        ++m_client_generations.ensure(client_child_id, [] { return 0uz; });
    return was_removed;
}

void ImageDecoderManager::process_did_release_decoder(u64 client_child_id, u64 client_generation)
{
    if (client_generation == this->client_generation(client_child_id))
        process_did_release_decoder(client_child_id);
}

void ImageDecoderManager::process_did_release_decoder(u64 client_child_id)
{
    m_instances.remove_all_matching([&](auto& instance) {
        auto count = instance.client_counts.find(client_child_id);
        if (count == instance.client_counts.end())
            return false;
        if (--count->value > 0)
            return false;
        instance.client_counts.remove(count);
        return instance.client_counts.is_empty();
    });
}

void ImageDecoderManager::process_did_exit(u64 child_id)
{
    m_client_generations.remove(child_id);

    // NB: Letting go of a decoder that exited also closes the channels of any clients that were sent to it, which it can no
    //     longer take up, so that those clients learn they have no decoder.
    m_instances.remove_all_matching([&](auto& instance) {
        if (instance.control_client->child_id() == child_id) {
            did_lose_decoder(instance);
            return true;
        }
        return instance.client_counts.remove(child_id) && instance.client_counts.is_empty();
    });
}

}
