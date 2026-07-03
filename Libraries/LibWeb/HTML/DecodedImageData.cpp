/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::HTML {

void DecodedImageData::Client::register_with_decoded_image_data_if_needed()
{
    auto const& image_data = decoded_image_data();

    if (!image_data)
        return;

    image_data->m_clients.set(this);

    image_data->on_client_registered();
}

void DecodedImageData::Client::unregister_with_decoded_image_data_if_needed()
{
    auto const& image_data = decoded_image_data();

    if (!image_data)
        return;

    image_data->m_clients.remove(this);
}

DecodedImageData::DecodedImageData() = default;

DecodedImageData::~DecodedImageData() = default;

Optional<Painting::DisplayListResource> DecodedImageData::record_display_list(Gfx::IntSize, Painting::DisplayListResourceStorage&) const
{
    return {};
}

void DecodedImageData::notify_clients_did_update()
{
    for (auto* client : m_clients)
        client->decoded_image_data_did_update();
}

}
