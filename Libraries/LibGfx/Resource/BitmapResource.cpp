/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/Environment.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImagePayload.h>

namespace Gfx {

static bool is_bitmap_logging_enabled()
{
    static bool const enabled = [] {
        auto value = Core::Environment::get("PAINT_SERVER_LOG"sv);
        if (!value.has_value())
            return false;
        return (value->to_number<u32>().value_or(0) & 4u) != 0;
    }();
    return enabled;
}

static Optional<BitmapFormat> image_bitmap_format_for_decoded_image_frame(DecodedImageFrame const& frame)
{
    switch (frame.bitmap().format()) {
    case BitmapFormat::RGBA8888:
        return BitmapFormat::RGBA8888;
    case BitmapFormat::RGBx8888:
        return BitmapFormat::RGBx8888;
    case BitmapFormat::BGRA8888:
        return BitmapFormat::BGRA8888;
    case BitmapFormat::BGRx8888:
        return BitmapFormat::BGRx8888;
    case BitmapFormat::RGBAF16:
    default:
        break;
    }

    return {};
}

static Optional<BitmapInfo> bitmap_info_for_decoded_image_frame(DecodedImageFrame const& frame)
{
    auto bitmap_format = image_bitmap_format_for_decoded_image_frame(frame);
    if (!bitmap_format.has_value())
        return {};

    u32 const row_bytes = static_cast<u32>(frame.bitmap().pitch());
    return BitmapInfo {
        .size = frame.size(),
        .row_bytes = row_bytes,
        .mip_level_count = 1,
        .sample_count = 1,
        .tiling_modifier = 0,
        .pixel_format = *bitmap_format,
        .color_space = frame.color_space().is_linear() ? BitmapColorSpace::Linear : BitmapColorSpace::SRGB,
        .alpha_type = frame.bitmap().alpha_type(),
        .origin = BitmapOrigin::TopLeft,
    };
}

static ResourceInfo bitmap_resource_info(ResourceID resource_id)
{
    ResourceInfo info;
    info.resource_id = resource_id;
    return info;
}

BitmapRegistry::BitmapRegistry() = default;
BitmapRegistry::BitmapRegistry(BitmapRegistry&&) = default;
BitmapRegistry& BitmapRegistry::operator=(BitmapRegistry&&) = default;
BitmapRegistry::~BitmapRegistry() = default;

ErrorOr<SharedImage*> BitmapRegistry::ensure_shared_image(ResourceID resource_id, DecodedImageFrame const& frame)
{
    auto existing_shared_image = m_shared_images.find(resource_id);
    if (existing_shared_image != m_shared_images.end())
        return existing_shared_image->value.ptr();

    auto description = bitmap_info_for_decoded_image_frame(frame);
    if (!description.has_value())
        return Error::from_string_literal("BitmapRegistry: unsupported bitmap pixel format");

    SharedImage shared_image = SharedImage::create(*description);
    if (shared_image.is_shareable_bitmap_backed())
        shared_image.set_color_space(frame.color_space());

    auto payload = shared_image.export_payload();
    auto upload_result = upload_decoded_image_frame_to_shared_image(frame, payload);
    if (upload_result.is_error())
        return upload_result.release_error();

    m_shared_images.set(resource_id, make<SharedImage>(move(shared_image)));
    auto shared_image_it = m_shared_images.find(resource_id);
    VERIFY(shared_image_it != m_shared_images.end());
    return shared_image_it->value.ptr();
}

Optional<ResourceID> BitmapRegistry::register_bitmap(ResourceID resource_id, DecodedImageFrame const& frame)
{
    if (resource_id == 0 || resource_id.type() != WireResourceType::Bitmap) {
        if (is_bitmap_logging_enabled()) {
            dbgln("BitmapRegistry: rejected bitmap registration resource_id={} type={} size={}x{}",
                resource_id,
                to_underlying(resource_id.type()),
                frame.size().width(),
                frame.size().height());
        }
        return {};
    }

    auto shared_image = ensure_shared_image(resource_id, frame);
    if (shared_image.is_error()) {
        if (is_bitmap_logging_enabled()) {
            dbgln("BitmapRegistry: shared image creation failed resource_id={} size={}x{} error={}",
                resource_id,
                frame.size().width(),
                frame.size().height(),
                shared_image.error());
        }
        return {};
    }

    auto shared_image_payload = shared_image.value()->export_payload();
    auto const& shared_image_info = shared_image_payload.info();
    m_last_infos.set(resource_id, shared_image_info);

    if (!m_resources.contains(resource_id)) {
        ResourceTransfer transfer;
        transfer.info = bitmap_resource_info(resource_id);
        transfer.shared_image_payload = make<SharedImagePayload>(move(shared_image_payload));

        m_pending_transfers.append(move(transfer));

        if (is_bitmap_logging_enabled()) {
            dbgln("BitmapRegistry: queued bitmap resource_id={} size={}x{} row_bytes={}",
                resource_id,
                shared_image_info.size.width(),
                shared_image_info.size.height(),
                shared_image_info.row_bytes);
        }

        m_resources.set(resource_id);
    }

    return resource_id;
}

void BitmapRegistry::clear()
{
    m_shared_images.clear();
    m_resources.clear();
    m_pending_transfers.clear();
    m_last_infos.clear();
}

void BitmapRegistry::invalidate_resource(ResourceID resource_id)
{
    m_shared_images.remove(resource_id);
    m_resources.remove(resource_id);
    m_last_infos.remove(resource_id);

    for (size_t index = m_pending_transfers.size(); index-- > 0;) {
        if (m_pending_transfers[index].info.resource_id == resource_id)
            m_pending_transfers.remove(index);
    }
}

}
