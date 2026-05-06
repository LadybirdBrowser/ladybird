/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <LibCore/System.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/Policy.h>
#include <PaintServer/Painter.h>
#include <PaintServer/RenderClient/RenderClientConnection.h>
#include <PaintServer/RenderClient/ResourceManager.h>
#include <PaintServer/Server.h>
#include <core/SkImage.h>
#include <errno.h>
#include <sys/mman.h>

namespace PaintServer {

class SharedMemoryRegion {
    AK_MAKE_NONCOPYABLE(SharedMemoryRegion);

public:
    SharedMemoryRegion() = default;
    SharedMemoryRegion(SharedMemoryRegion&&);
    SharedMemoryRegion& operator=(SharedMemoryRegion&&);
    ~SharedMemoryRegion();

    bool remap(IPC::File file_handle, size_t mapping_size);
    size_t size() const { return m_size; }
    Optional<ReadonlyBytes> slice(size_t offset, size_t size) const;

private:
    void clear();

    int m_fd { -1 };
    size_t m_size { 0 };
    u8* m_mapping { nullptr };
};

class PendingResourceUpload : public RefCounted<PendingResourceUpload> {
public:
    explicit PendingResourceUpload(SharedMemoryRegion mapping)
        : m_mapping(move(mapping))
    {
    }

    size_t size() const { return m_mapping.size(); }

    bool is_invalidated() const { return m_is_invalidated.load(AK::MemoryOrder::memory_order_relaxed); }
    void invalidate() { m_is_invalidated.store(true, AK::MemoryOrder::memory_order_relaxed); }

    Optional<ReadonlyBytes> bytes() const
    {
        if (is_invalidated())
            return {};
        return m_mapping.slice(0, m_mapping.size());
    }

private:
    SharedMemoryRegion m_mapping;
    Atomic<bool> m_is_invalidated { false };
};

class ResourceManager::Impl {
public:
    struct ImageRecord {
        Gfx::SharedImage shared_image;
        RefPtr<Gfx::PaintingSurface> canvas_painting_surface;
        bool is_uploaded { false };
        sk_sp<SkImage> sk_image;
    };

    HashMap<ArenaID, OwnPtr<SharedMemoryRegion>> arenas;
    size_t total_arena_size { 0 };

    HashMap<u64, NonnullRefPtr<PendingResourceUpload>> pending_resource_uploads;
    size_t total_pending_resource_upload_size { 0 };
    u64 next_pending_resource_upload_id { 1 };

    HashMap<ImageID, ImageRecord> images;
};

static ErrorOr<void> validate_bitmap_resource_payload_registration(Gfx::ResourceInfo const& descriptor, Gfx::SharedImagePayload const& payload)
{
    if (descriptor.resource_id.type() != Gfx::WireResourceType::Bitmap)
        return Error::from_string_literal("ResourceManager: unsupported bitmap resource type");
    if (descriptor.resource_id == 0)
        return Error::from_string_literal("ResourceManager: bitmap resource id must be non-zero");

    if (payload.info().size.is_empty())
        return Error::from_string_literal("ResourceManager: bitmap resource dimensions must be non-zero");

    return {};
}

ErrorOr<void> ResourceManager::store_image(Impl& impl, ImageID image_id, Gfx::SharedImage shared_image, bool is_uploaded)
{
    if (image_id == 0)
        return Error::from_string_literal("ResourceManager: image_id must be non-zero");
    if (impl.images.contains(image_id))
        return Error::from_string_literal("ResourceManager: image_id is already registered");

    impl.images.set(image_id, Impl::ImageRecord { move(shared_image), nullptr, is_uploaded, nullptr });
    return {};
}

ErrorOr<Gfx::SharedImagePayload> ResourceManager::allocate_headless_content_image(Impl& impl, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
    if (impl.images.contains(image_id))
        return Error::from_string_literal("ResourceManager: image_id is already allocated");

    if (format != Gfx::BitmapFormat::BGRA8888 && format != Gfx::BitmapFormat::RGBA8888)
        return Error::from_string_literal("ResourceManager: unsupported headless content image pixel format");

    auto content_image = Gfx::SharedImage::create({
        .size = size,
        .pixel_format = format,
        .color_space = Gfx::BitmapColorSpace::SRGB,
        .alpha_type = Gfx::BitmapAlpha::Premultiplied,
        .origin = Gfx::BitmapOrigin::TopLeft,
    });
    auto payload = content_image.export_payload();
    TRY(store_image(impl, image_id, move(content_image), false));
    return payload;
}

static bool incoming_size_exceeds_available_capacity(size_t incoming_size, size_t current_total_size, size_t replaced_size, size_t total_capacity)
{
    return incoming_size > total_capacity - (current_total_size - replaced_size);
}

SharedMemoryRegion::SharedMemoryRegion(SharedMemoryRegion&& other)
    : m_fd(exchange(other.m_fd, -1))
    , m_size(exchange(other.m_size, 0))
    , m_mapping(exchange(other.m_mapping, nullptr))
{
}

SharedMemoryRegion& SharedMemoryRegion::operator=(SharedMemoryRegion&& other)
{
    if (this == &other)
        return *this;

    clear();
    m_fd = exchange(other.m_fd, -1);
    m_size = exchange(other.m_size, 0);
    m_mapping = exchange(other.m_mapping, nullptr);
    return *this;
}

SharedMemoryRegion::~SharedMemoryRegion()
{
    clear();
}

bool SharedMemoryRegion::remap(IPC::File file_handle, size_t mapping_size)
{
    if (mapping_size == 0)
        return false;

    int const fd = file_handle.take_fd();
    if (fd < 0) {
        dbgln("ResourceManager: take_fd returned invalid fd");
        return false;
    }

    ArmedScopeGuard close_fd = [&] {
        auto close_result = Core::System::close(fd);
        if (close_result.is_error())
            dbgln("ResourceManager: close failed fd={} error={}", fd, close_result.error());
    };

    void* const mapped_region = mmap(nullptr, mapping_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped_region == MAP_FAILED) {
        dbgln("ResourceManager: mmap failed fd={} size={} errno={}", fd, mapping_size, errno);
        return false;
    }

    close_fd.disarm();
    clear();
    m_fd = fd;
    m_size = mapping_size;
    m_mapping = static_cast<u8*>(mapped_region);
    return true;
}

void SharedMemoryRegion::clear()
{
    if (m_mapping) {
        if (::munmap(m_mapping, m_size) < 0)
            dbgln("ResourceManager: munmap failed fd={} size={} errno={}", m_fd, m_size, errno);
    }

    if (m_fd >= 0) {
        auto close_result = Core::System::close(m_fd);
        if (close_result.is_error())
            dbgln("ResourceManager: close failed fd={} error={}", m_fd, close_result.error());
    }

    m_fd = -1;
    m_size = 0;
    m_mapping = nullptr;
}

Optional<ReadonlyBytes> SharedMemoryRegion::slice(size_t offset, size_t size) const
{
    if (!m_mapping || m_size == 0 || offset > m_size || size > m_size - offset)
        return {};
    return ReadonlyBytes { m_mapping + offset, size };
}

ResourceManager::ResourceManager()
    : m_impl(make<Impl>())
{
}

ResourceManager::~ResourceManager()
{
    reset();
}

void ResourceManager::reset()
{
    m_impl->arenas.clear();
    m_impl->total_arena_size = 0;

    for (auto& it : m_impl->pending_resource_uploads) {
        it.value->invalidate();
    }
    m_impl->pending_resource_uploads.clear();
    m_impl->total_pending_resource_upload_size = 0;
    m_impl->next_pending_resource_upload_id = 1;
}

Optional<ReadonlyBytes> ResourceManager::arena_slice(ArenaID arena_id, size_t offset, size_t size) const
{
    auto it = m_impl->arenas.find(arena_id);
    if (it == m_impl->arenas.end())
        return {};
    return it->value->slice(offset, size);
}

ErrorOr<void> ResourceManager::register_bitmap_resource(ConnectionID connection_id, Gfx::ResourceInfo const& descriptor, Gfx::SharedImagePayload image_payload)
{
    VERIFY(descriptor.resource_id.type() == Gfx::WireResourceType::Bitmap);

    TRY(validate_bitmap_resource_payload_registration(descriptor, image_payload));
    auto shared_image = Gfx::SharedImage::import_from_payload(move(image_payload));
    ImageID const image_id = descriptor.resource_id.raw();
    bool existing = m_impl->images.contains(image_id);
    if (existing)
        destroy_image(image_id);
    if (is_logging_enabled(LOG_RESOURCE)) {
        auto verb = existing ? "replacing"sv : "registering"sv;
        dbgln("ResourceManager: {} bitmap resource image connection_id={} resource_id={} image_id={} size={}x{} format={} alpha={}",
            verb,
            connection_id,
            descriptor.resource_id,
            image_id,
            shared_image.info().size.width(),
            shared_image.info().size.height(),
            Gfx::bitmap_format_name(shared_image.info().pixel_format),
            to_underlying(shared_image.info().alpha_type));
    }

    TRY(store_image(*m_impl, image_id, move(shared_image), true));
    return {};
}

void ResourceManager::unregister_bitmap_resource(ResourceID resource_id)
{
    if (resource_id.type() != Gfx::WireResourceType::Bitmap)
        return;

    destroy_image(resource_id.raw());
}

ErrorOr<Gfx::SharedImagePayload> ResourceManager::allocate_content_image(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Painter& painter, bool is_headless)
{
    auto existing_image = m_impl->images.find(image_id);
    if (existing_image != m_impl->images.end()) {
        auto const& info = existing_image->value.shared_image.info();
        if (info.size == size && info.pixel_format == format)
            return existing_image->value.shared_image.export_payload();

        destroy_image(image_id);
    }

    if (is_headless) {
        auto content_image = TRY(allocate_headless_content_image(*m_impl, image_id, size, format));
        return content_image;
    }

    auto shared_image = TRY(painter.create_content_image(image_id, size, format));
    auto payload = shared_image.export_payload();
    TRY(store_image(*m_impl, image_id, move(shared_image), false));
    return payload;
}

ErrorOr<void> ResourceManager::import_content_image(ImageID image_id, Gfx::SharedImagePayload image_payload)
{
    if (image_id == 0)
        return Error::from_string_literal("ResourceManager: invalid content image id");

    if (m_impl->images.contains(image_id))
        destroy_image(image_id);

    auto shared_image = Gfx::SharedImage::import_from_payload(move(image_payload));
    TRY(store_image(*m_impl, image_id, move(shared_image), true));
    return {};
}

void ResourceManager::complete_image_upload(ImageID image_id, bool success)
{
    auto it = m_impl->images.find(image_id);
    if (it == m_impl->images.end())
        return;

    if (!success) {
        m_impl->images.remove(it);
        return;
    }
    it->value.sk_image = nullptr;
    it->value.is_uploaded = true;
}

void ResourceManager::destroy_image(ImageID image_id)
{
    if (image_id == 0)
        return;

    m_impl->images.remove(image_id);
}

void ResourceManager::destroy_all_images()
{
    m_impl->images.clear();
}

sk_sp<SkImage> ResourceManager::resolve_image(ResourceID, ImageID image_id, Painter& painter, bool is_headless)
{
    auto it = m_impl->images.find(image_id);
    if (it == m_impl->images.end() || !it->value.is_uploaded)
        return nullptr;

    if (!it->value.sk_image) {
        if (is_headless) {
            NonnullRefPtr<Gfx::PaintingSurface> painting_surface = Gfx::PaintingSurface::wrap_bitmap(*it->value.shared_image.bitmap(), it->value.shared_image.color_space(), it->value.shared_image.info().color_space);
            auto sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
            if (!sk_image)
                return nullptr;

            it->value.sk_image = move(sk_image);
        } else {
            auto sk_image = painter.snapshot_shared_image(it->value.shared_image);
            if (!sk_image)
                return nullptr;

            it->value.sk_image = move(sk_image);
        }
    }

    return it->value.sk_image;
}

Gfx::SharedImage* ResourceManager::shared_image(ImageID image_id)
{
    auto it = m_impl->images.find(image_id);
    if (it == m_impl->images.end())
        return nullptr;
    return &it->value.shared_image;
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> ResourceManager::canvas_painting_surface(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Painter& painter, bool is_headless)
{
    auto image_record = m_impl->images.find(image_id);
    if (image_record != m_impl->images.end()) {
        auto const& info = image_record->value.shared_image.info();
        if (info.size != size || info.pixel_format != format) {
            destroy_image(image_id);
            image_record = m_impl->images.end();
        }
    }

    if (image_record == m_impl->images.end()) {
        auto allocation_result = allocate_content_image(image_id, size, format, painter, is_headless);
        if (allocation_result.is_error())
            return allocation_result.release_error();
        image_record = m_impl->images.find(image_id);
        if (image_record == m_impl->images.end())
            return Error::from_string_literal("ResourceManager: image allocation did not create a record");
    }

    if (!image_record->value.canvas_painting_surface) {
        auto painting_surface = painter.create_content_image_painting_surface(image_record->value.shared_image, OffscreenBackendPreference::PreferGPU);
        if (painting_surface.is_error())
            return painting_surface.release_error();
        image_record->value.canvas_painting_surface = painting_surface.release_value();
    }

    return NonnullRefPtr<Gfx::PaintingSurface>(*image_record->value.canvas_painting_surface);
}

void ResourceManager::register_resource_from_shared_blob(RenderClientConnection& connection, SurfaceID surface_id, Gfx::ResourceInfo descriptor, IPC::File blob_handle, size_t blob_size, ReleaseToken release_token)
{
    if (incoming_size_exceeds_available_capacity(blob_size, m_impl->total_pending_resource_upload_size, 0, MAX_TOTAL_BLOBS_SIZE)) {
        size_t const available_capacity = MAX_TOTAL_BLOBS_SIZE - m_impl->total_pending_resource_upload_size;
        dbgln("ResourceManager: upload too large {} size={} available_capacity={}", descriptor.resource_id, blob_size, available_capacity);
        connection.complete_resource_registration_at_ingress(surface_id, descriptor.resource_id, release_token, false);
        return;
    }

    SharedMemoryRegion blob;
    if (!blob.remap(move(blob_handle), blob_size)) {
        dbgln("ResourceManager: upload remap failed {} size={}", descriptor.resource_id, blob_size);
        connection.complete_resource_registration_at_ingress(surface_id, descriptor.resource_id, release_token, false);
        return;
    }

    u64 upload_id = m_impl->next_pending_resource_upload_id++;
    if (upload_id == 0)
        upload_id = m_impl->next_pending_resource_upload_id++;

    NonnullRefPtr<PendingResourceUpload> pending_upload = adopt_ref(*new PendingResourceUpload(move(blob)));
    m_impl->pending_resource_uploads.set(upload_id, pending_upload);
    m_impl->total_pending_resource_upload_size += pending_upload->size();

    RefPtr<RenderClientConnection> protected_connection { connection };

    (void)connection.m_server.post_to_compositor_thread([pending_upload = move(pending_upload), protected_connection = move(protected_connection), surface_id, descriptor, upload_id, release_token]() mutable {
        bool success = false;

        auto log_invalidated = [&] {
            if (is_logging_enabled(LOG_RESOURCE))
                dbgln("ResourceManager: upload invalidated {} size={}", descriptor.resource_id, pending_upload->size());
        };

        auto resource_data = pending_upload->bytes();
        if (pending_upload->is_invalidated() || !resource_data.has_value()) {
            log_invalidated();
        } else {
            switch (descriptor.resource_id.type()) {
            case Gfx::WireResourceType::SkTypeface: {
                auto result = protected_connection->m_compositor_font_cache.register_font(surface_id, descriptor.resource_id, resource_data.value(), descriptor.font.ttc_index);
                success = !result.is_error();
                if (is_logging_enabled(LOG_RESOURCE) && success)
                    dbgln("ResourceManager: registered {} size={}", descriptor.resource_id, resource_data->size());
                else if (!success)
                    dbgln("ResourceManager: register failed {} size={} error={}", descriptor.resource_id, resource_data->size(), result.error());

                break;
            }
            case Gfx::WireResourceType::LocalFont: {
                auto result = protected_connection->m_compositor_font_cache.register_local_font(surface_id, descriptor.resource_id, resource_data.value());
                success = !result.is_error();
                if (is_logging_enabled(LOG_RESOURCE) && success)
                    dbgln("ResourceManager: registered {} size={}", descriptor.resource_id, resource_data->size());
                else if (!success)
                    dbgln("ResourceManager: register failed {} size={} error={}", descriptor.resource_id, resource_data->size(), result.error());
                break;
            }
            default:
                dbgln("ResourceManager: unsupported resource type {} size={}", descriptor.resource_id, resource_data->size());
            }
        }

        protected_connection->m_server.post_to_main_thread([protected_connection = move(protected_connection), surface_id, resource_id = descriptor.resource_id, upload_id, release_token, success]() mutable {
            auto upload = protected_connection->m_resource_manager.m_impl->pending_resource_uploads.take(upload_id);
            if (upload.has_value()) {
                upload.value()->invalidate();
                protected_connection->m_resource_manager.m_impl->total_pending_resource_upload_size -= upload.value()->size();
            }
            protected_connection->complete_resource_registration_at_ingress(surface_id, resource_id, release_token, success);
        });
    });
}

bool ResourceManager::register_arena(ArenaID arena_id, IPC::File arena_handle, size_t arena_size)
{
    size_t previous_arena_size = 0;
    if (auto it = m_impl->arenas.find(arena_id); it != m_impl->arenas.end())
        previous_arena_size = it->value->size();

    if (incoming_size_exceeds_available_capacity(arena_size, m_impl->total_arena_size, previous_arena_size, MAX_TOTAL_ARENA_SIZE)) {
        size_t const available_capacity = MAX_TOTAL_ARENA_SIZE - (m_impl->total_arena_size - previous_arena_size);
        if (is_logging_enabled(LOG_RESOURCE))
            dbgln("ResourceManager: shared arena too large id={} size={} available_capacity={}", arena_id, arena_size, available_capacity);
        return false;
    }

    auto arena = make<SharedMemoryRegion>();
    if (!arena->remap(move(arena_handle), arena_size)) {
        if (is_logging_enabled(LOG_RESOURCE))
            dbgln("ResourceManager: shared arena remap failed id={} size={}", arena_id, arena_size);
        return false;
    }

    m_impl->arenas.set(arena_id, move(arena));
    m_impl->total_arena_size = (m_impl->total_arena_size - previous_arena_size) + arena_size;

    if (is_logging_enabled(LOG_RESOURCE))
        dbgln("ResourceManager: registered type=arena id={} size={} replaced_size={} total_size={}", arena_id, arena_size, previous_arena_size, m_impl->total_arena_size);

    return true;
}

void ResourceManager::unregister_arena(ArenaID arena_id)
{
    auto arena = m_impl->arenas.take(arena_id);
    if (!arena.has_value())
        return;

    m_impl->total_arena_size -= arena.value()->size();

    if (is_logging_enabled(LOG_RESOURCE))
        dbgln("ResourceManager: unregistered type=arena id={} size={} total_size={}", arena_id, arena.value()->size(), m_impl->total_arena_size);
}

}
