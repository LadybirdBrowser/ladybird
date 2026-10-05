/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Position.h>

namespace JS {

// The source code that scripts, modules and functions are compiled from. A pointer to it is the Rust runtime's source
// code itself, which the runtime and its embedder share by reference counting, as RefPtr<SourceCode const> does in the
// C++ runtime. Only the runtime creates and destroys it, and only the VM's thread may reference or release it.
class JS_API SourceCode {
    AK_MAKE_NONCOPYABLE(SourceCode);
    AK_MAKE_NONMOVABLE(SourceCode);

public:
    static NonnullRefPtr<SourceCode const> create(Utf16String filename, Utf16String code);

    // The C++ runtime decodes the bytes when the code is first needed; this decodes them right away.
    static NonnullRefPtr<SourceCode const> create(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes);

    Utf16String const& filename() const;
    Utf16String const& code() const;
    size_t length_in_code_units() const;

    // The code as UTF-16 code units, which stay valid while the source code lives, so another thread may read them.
    u16 const* utf16_data() const;

    Utf16String source_text_from_offsets(size_t start_offset, size_t length) const;

    void ref() const;
    void unref() const;

    SourceCode() = delete;
    ~SourceCode() = delete;
};

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<JS::SourceCode> = true;
