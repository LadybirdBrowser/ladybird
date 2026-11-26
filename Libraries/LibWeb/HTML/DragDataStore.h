/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>

namespace Web::HTML {

struct RustDragDataStore;

class DragDataStore final : public RefCounted<DragDataStore> {
public:
    enum class Mode : u8 {
        ReadWrite,
        ReadOnly,
        Protected,
    };

    struct Item {
        Utf16FlyString type_string;
        Utf16String data;
        bool is_file;
    };

    static NonnullRefPtr<DragDataStore> create();
    ~DragDataStore();

    void add_text(Utf16FlyString const& type_string, Utf16String const& data);
    void add_file(Utf16FlyString const& type_string, ByteBuffer data, Utf16String const& name);

    Item item_at(size_t index) const;
    ErrorOr<String> file_data_as_base64(size_t index) const;
    size_t size() const;
    bool has_text_item() const;

    void set_mode(Mode);

    RustDragDataStore const* rust_store() const { return m_store; }

private:
    explicit DragDataStore(RustDragDataStore*);

    RustDragDataStore* m_store;
};

}
