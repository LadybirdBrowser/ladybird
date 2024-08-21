/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/FlyString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>

namespace Web::HTML {

struct DragDataStoreItem {
    enum class Kind {
        Text,
        File,
    };

    // https://html.spec.whatwg.org/multipage/dnd.html#the-drag-data-item-kind
    Kind kind { Kind::Text };

    // https://html.spec.whatwg.org/multipage/dnd.html#the-drag-data-item-type-string
    String type_string;

    ByteBuffer data;
    ByteString file_name;
};

// https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store
class DragDataStore : public RefCounted<DragDataStore> {
public:
    enum class Mode {
        ReadWrite,
        ReadOnly,
        Protected,
    };

    static NonnullRefPtr<DragDataStore> create();
    ~DragDataStore();

    void add_item(DragDataStoreItem item) { m_item_list.append(move(item)); }
    ReadonlySpan<DragDataStoreItem> item_list() const { return m_item_list; }
    size_t size() const { return m_item_list.size(); }
    bool has_text_item() const;

    Mode mode() const { return m_mode; }
    void set_mode(Mode mode) { m_mode = mode; }

    FlyString allowed_effects_state() const { return m_allowed_effects_state; }
    void set_allowed_effects_state(FlyString allowed_effects_state) { m_allowed_effects_state = move(allowed_effects_state); }

private:
    DragDataStore();

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-item-list
    Vector<DragDataStoreItem> m_item_list;

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-default-feedback
    String m_default_feedback;

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-bitmap
    RefPtr<Gfx::Bitmap> m_bitmap;

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-hot-spot-coordinate
    Gfx::IntPoint m_hot_spot_coordinate;

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-mode
    Mode m_mode { Mode::Protected };

    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-allowed-effects-state
    FlyString m_allowed_effects_state;
};

}
