#pragma once

#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Stream.h>
#include <AK/Vector.h>
#include <LibHTTP/HeaderList.h>

namespace RequestServer {

enum class ContentEncoding : u8 {
    Gzip,
    Deflate,
    Brotli,
    Zstd,
};

// parse the Content-Encoding header and return the chain of encoding methods to be "undone"
Vector<ContentEncoding> parse_content_encoding(HTTP::HeaderList const&);

class BodyDecoder {
public:
    static NonnullOwnPtr<BodyDecoder> create_pass_through();
    static ErrorOr<NonnullOwnPtr<BodyDecoder>> create(Vector<ContentEncoding> const&);

    ~BodyDecoder();

    ErrorOr<void> push(ReadonlyBytes wire_bytes, AllocatingMemoryStream& out);

    ErrorOr<void> finish(AllocatingMemoryStream& out);

    bool is_pass_through() const { return !m_top; }

private:
    class FeederStream;

    BodyDecoder() = default;
    BodyDecoder(NonnullOwnPtr<FeederStream>, NonnullOwnPtr<Stream>);

    OwnPtr<FeederStream> m_feeder;
    OwnPtr<Stream> m_top;
};

}
