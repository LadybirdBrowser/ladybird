/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <woff2/decode.h>

namespace WOFF2 {

class WOFF2ByteBufferOut final : public woff2::WOFF2Out {
public:
    explicit WOFF2ByteBufferOut(ByteBuffer& buffer)
        : m_buffer(buffer)
    {
    }

    // Append n bytes of data from buf.
    // Return true if all written, false otherwise.
    virtual bool Write(void const* data, size_t size) override
    {
        auto result = m_buffer.try_append(static_cast<u8 const*>(data), size);
        if (result.is_error()) {
            VERIFY_NOT_REACHED();
        }
        return true;
    }

    // Write n bytes of data from buf at offset.
    // Return true if all written, false otherwise.
    virtual bool Write(void const* data, size_t offset, size_t n) override
    {
        if (Checked<size_t>::addition_would_overflow(offset, n)) {
            return false;
        }
        if (offset + n > m_buffer.size()) {
            if (m_buffer.try_resize(offset + n).is_error()) {
                return false;
            }
        }
        memcpy(m_buffer.offset_pointer(offset), data, n);
        return true;
    }

    virtual size_t Size() override
    {
        return m_buffer.size();
    }

private:
    ByteBuffer& m_buffer;
};

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes)
{
    auto ttf_buffer = TRY(ByteBuffer::create_uninitialized(0));
    auto output = WOFF2ByteBufferOut { ttf_buffer };
    auto result = woff2::ConvertWOFF2ToTTF(bytes.data(), bytes.size(), &output);
    if (!result) {
        return Error::from_string_literal("Failed to convert the WOFF2 font to TTF");
    }

    auto font_data = Gfx::FontData::create_from_byte_buffer(move(ttf_buffer));
    auto input_font = TRY(Gfx::Typeface::try_load_from_font_data(move(font_data)));
    return input_font;
}

}
