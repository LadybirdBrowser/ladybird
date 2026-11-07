/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

ErrorOr<IPC::TransportHandle> ImageDecoderManager::connect_new_client(u64 client_child_id, IsPrivate)
{
    // NB: A decoder that died is of no use to anyone, whether or not its clients have noticed yet.
    m_instances.remove_all_matching([](auto const& instance) { return !instance.control_client->is_open(); });

    auto instance = m_instances.first_matching([&](auto const& instance) {
        return instance.client_child_ids.contains(client_child_id);
    });
    auto channel = TRY(IPC::TransportHandle::create_pair());
    if (!instance.has_value()) {
        m_instances.append({ .control_client = TRY(launch_image_decoder_process(client_child_id)), .client_child_ids = {} });
        instance = m_instances.last();
    }

    instance->control_client->async_connect_new_client(channel.first);
    instance->client_child_ids.set(client_child_id);
    return move(channel.second);
}

Optional<IPC::TransportHandle> ImageDecoderManager::connect_requesting_process(u64 client_child_id, IsPrivate is_private)
{
    // NB: A request can still be queued when the process's exit is handled. Connecting it then would leave a client that
    //     never exits, which would keep its decoder alive for as long as the Browser runs.
    if (!Application::process_manager().find_child(client_child_id).has_value())
        return {};

    auto handle = connect_new_client(client_child_id, is_private);
    if (handle.is_error()) {
        warnln("Unable to connect an ImageDecoder client: {}", handle.error());
        return {};
    }
    return handle.release_value();
}

void ImageDecoderManager::process_did_exit(u64 child_id)
{
    // NB: Letting go of a decoder that exited also closes the channels of any clients that were sent to it, which it can no
    //     longer take up, so that those clients learn they have no decoder.
    m_instances.remove_all_matching([&](auto& instance) {
        if (instance.control_client->child_id() == child_id)
            return true;
        return instance.client_child_ids.remove(child_id) && instance.client_child_ids.is_empty();
    });
}

}
