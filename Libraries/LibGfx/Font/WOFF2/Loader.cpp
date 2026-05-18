/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Checked.h>
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

    virtual bool Write(void const* data, size_t size) override
    {
        if (m_buffer.try_append(static_cast<u8 const*>(data), size).is_error())
            return false;
        return true;
    }

    virtual bool Write(void const* data, size_t offset, size_t n) override
    {
        if (Checked<size_t>::addition_would_overflow(offset, n))
            return false;
        if (offset + n > m_buffer.size()) {
            if (m_buffer.try_resize(offset + n).is_error())
                return false;
        }
        if (n > 0)
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

ErrorOr<Core::AnonymousBuffer> convert_to_ttf(ReadonlyBytes bytes)
{
    auto ttf_buffer = TRY(ByteBuffer::create_uninitialized(0));
    auto output = WOFF2ByteBufferOut { ttf_buffer };
    if (!woff2::ConvertWOFF2ToTTF(bytes.data(), bytes.size(), &output))
        return Error::from_string_literal("Failed to convert the WOFF2 font to TTF");

    auto anonymous_buffer = TRY(Core::AnonymousBuffer::create_with_size(ttf_buffer.size()));
    if (!ttf_buffer.is_empty())
        memcpy(anonymous_buffer.data<void>(), ttf_buffer.data(), ttf_buffer.size());
    return anonymous_buffer;
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_bytes(ReadonlyBytes bytes)
{
    return Gfx::Typeface::try_load_from_anonymous_buffer(TRY(convert_to_ttf(bytes)));
}

}
