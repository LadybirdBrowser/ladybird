/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/ListOfAvailableImages.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ListOfAvailableImages);

static u64 s_next_available_image_cache_touch_serial;

ListOfAvailableImages::ListOfAvailableImages() = default;
ListOfAvailableImages::~ListOfAvailableImages() = default;

bool ListOfAvailableImages::Key::operator==(Key const& other) const
{
    return url == other.url && mode == other.mode && origin == other.origin;
}

u32 ListOfAvailableImages::Key::hash() const
{
    return cached_hash.ensure([&] {
        u32 url_hash = url.hash();
        u32 mode_hash = static_cast<u32>(mode);
        u32 origin_hash = 0;
        if (origin.has_value())
            origin_hash = Traits<URL::Origin>::hash(origin.value());
        return pair_int_hash(url_hash, pair_int_hash(mode_hash, origin_hash));
    });
}

void ListOfAvailableImages::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& it : m_images)
        visitor.visit(it.value->image_data);
}

void ListOfAvailableImages::add(Key const& key, GC::Ref<DecodedImageData> image_data, bool ignore_higher_layer_caching)
{
    auto cache_touch_serial = ++s_next_available_image_cache_touch_serial;
    m_images.set(key, make<Entry>(image_data, ignore_higher_layer_caching, cache_touch_serial));
}

void ListOfAvailableImages::remove(Key const& key)
{
    m_images.remove(key);
}

void ListOfAvailableImages::prune_to_limits(size_t external_memory_limit, size_t count_limit)
{
    struct CacheSize {
        size_t decoded_image_size { 0 };
        size_t decoded_image_count { 0 };
    };

    auto cache_size = [&] {
        CacheSize cache_size;
        for (auto const& it : m_images)
            cache_size.decoded_image_size += it.value->image_data->external_memory_size();
        cache_size.decoded_image_count = m_images.size();
        return cache_size;
    };

    auto size = cache_size();
    while (size.decoded_image_size > external_memory_limit || size.decoded_image_count > count_limit) {
        Optional<Key> least_recently_used_key;
        u64 least_recently_used_serial = NumericLimits<u64>::max();

        for (auto const& it : m_images) {
            if (it.value->cache_touch_serial >= least_recently_used_serial)
                continue;
            least_recently_used_key = it.key;
            least_recently_used_serial = it.value->cache_touch_serial;
        }

        if (!least_recently_used_key.has_value())
            break;

        m_images.remove(least_recently_used_key.value());

        auto new_size = cache_size();
        if (new_size.decoded_image_size == size.decoded_image_size
            && new_size.decoded_image_count == size.decoded_image_count)
            break;
        size = new_size;
    }
}

ListOfAvailableImages::Entry* ListOfAvailableImages::get(Key const& key)
{
    auto it = m_images.find(key);
    if (it == m_images.end())
        return nullptr;
    it->value->cache_touch_serial = ++s_next_available_image_cache_touch_serial;
    return it->value.ptr();
}

}
