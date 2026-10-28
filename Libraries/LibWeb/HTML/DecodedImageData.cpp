/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

void DecodedImageData::Client::register_with_decoded_image_data_if_needed()
{
    auto const& image_data = decoded_image_data();

    if (!image_data)
        return;

    image_data->m_clients.set(this);
    m_registered_image_data = image_data;

    image_data->on_client_registered();
}

// Clients unregister from their finalizers and destructors, when what leads to their current image data may already
// have been collected. So unregister from the image data we registered with, if it is still alive.
void DecodedImageData::Client::unregister_with_decoded_image_data_if_needed()
{
    if (auto image_data = m_registered_image_data.ptr())
        image_data->m_clients.remove(this);
    m_registered_image_data = nullptr;
}

DecodedImageData::DecodedImageData() = default;

DecodedImageData::~DecodedImageData() = default;

Optional<Compositing::DisplayListResource> DecodedImageData::record_display_list(Gfx::IntSize, CSS::PreferredColorScheme, Compositing::DisplayListResourceStorage&) const
{
    return {};
}

bool DecodedImageData::has_clients_that_may_render() const
{
    for (auto* client : m_clients) {
        if (client->decoded_image_data_may_be_rendered())
            return true;
    }
    return false;
}

void DecodedImageData::notify_clients_did_update()
{
    for (auto* client : m_clients)
        client->decoded_image_data_did_update();
}

}
