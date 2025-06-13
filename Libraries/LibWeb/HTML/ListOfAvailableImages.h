/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#list-of-available-images
class ListOfAvailableImages : public JS::Cell {
    GC_CELL(ListOfAvailableImages, JS::Cell);
    GC_DECLARE_ALLOCATOR(ListOfAvailableImages);

public:
    struct Key {
        URL::URL url;
        HTML::CORSSettingAttribute mode;
        Optional<URL::Origin> origin;

        [[nodiscard]] bool operator==(Key const& other) const;
        [[nodiscard]] u32 hash() const;

    private:
        mutable Optional<u32> cached_hash;
    };

    struct Entry {
        Entry(GC::Ref<DecodedImageData> image_data, bool ignore_higher_layer_caching)
            : image_data(move(image_data))
            , ignore_higher_layer_caching(ignore_higher_layer_caching)
        {
        }

        GC::Ref<DecodedImageData> image_data;
        bool ignore_higher_layer_caching { false };
    };

    ListOfAvailableImages();
    ~ListOfAvailableImages();

    void add(Key const&, GC::Ref<DecodedImageData>, bool ignore_higher_layer_caching);
    void remove(Key const&);
    [[nodiscard]] Entry* get(Key const&);

    void visit_edges(JS::Cell::Visitor& visitor) override;

private:
    HashMap<Key, NonnullOwnPtr<Entry>> m_images;
};

}

namespace AK {

template<>
struct Traits<Web::HTML::ListOfAvailableImages::Key> : public DefaultTraits<Web::HTML::ListOfAvailableImages::Key> {
    static unsigned hash(Web::HTML::ListOfAvailableImages::Key const& key)
    {
        return key.hash();
    }
    static bool equals(Web::HTML::ListOfAvailableImages::Key const& a, Web::HTML::ListOfAvailableImages::Key const& b)
    {
        return a == b;
    }
};

}
