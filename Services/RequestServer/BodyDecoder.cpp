#include <AK/Array.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <AK/StringView.h>
#include <LibCompress/Gzip.h>
#include <RequestServer/BodyDecoder.h>

namespace RequestServer {

class BodyDecoder::FeederStream : public Stream {
public:
    virtual ErrorOr<Bytes> read_some(Bytes bytes) override { return m_buffer.read_some(bytes); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes bytes) override { return m_buffer.write_some(bytes); }
    virtual bool is_eof() const override { return m_eof_marked && m_buffer.is_eof(); }
    virtual bool is_open() const override { return !m_eof_marked; }
    virtual void close() override { m_eof_marked = true; }

    void mark_eof() { m_eof_marked = true; }

private:
    AllocatingMemoryStream m_buffer;
    bool m_eof_marked { false };
};

static Optional<ContentEncoding> recognize_token(StringView token)
{
    // RFC 7231: https://www.iana.org/assignments/http-parameters/http-parameters.xhtml
    // Chrome implements: deflate, gzip, x-gzip, br, and zstd https://chromium.googlesource.com/chromium/src/+/refs/heads/main/net/filter/filter_source_stream.cc
    // libcurl implements the same set as Chrome

    if (token.equals_ignoring_ascii_case("gzip"sv) || token.equals_ignoring_ascii_case("x-gzip"sv))
        return ContentEncoding::Gzip;
    if (token.equals_ignoring_ascii_case("deflate"sv))
        return ContentEncoding::Deflate;
    if (token.equals_ignoring_ascii_case("br"sv))
        return ContentEncoding::Brotli;
    if (token.equals_ignoring_ascii_case("zstd"sv))
        return ContentEncoding::Zstd;
    return { };
}

Vector<ContentEncoding> parse_content_encoding(HTTP::HeaderList const& headers)
{
    Vector<ContentEncoding> encoding_chain;
    bool invalid_token_found = false;

    headers.for_each_header_value("Content-Encoding"sv, [&](StringView value) -> IterationDecision {
        value.for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView raw_token) -> IterationDecision {
            auto token = raw_token.trim(" \t"sv);

            // Skip empty or identity tokens
            if (token.is_empty() || token.equals_ignoring_ascii_case("identity"sv))
                return IterationDecision::Continue;

            auto encoding = recognize_token(token);

            if (!encoding.has_value()) {
                // Unknown encoding: pass through the raw body.
                invalid_token_found = true;
                return IterationDecision::Break;
            }

            encoding_chain.append(*encoding);
            return IterationDecision::Continue;
        });

        return invalid_token_found ? IterationDecision::Break : IterationDecision::Continue;
    });

    if (invalid_token_found) {
        return { };
    }

    return encoding_chain;
}

static ErrorOr<NonnullOwnPtr<Stream>> wrap_with_decoder(ContentEncoding encoding, MaybeOwned<Stream> upstream)
{
    switch (encoding) {
    case ContentEncoding::Gzip:
        return TRY(Compress::GzipDecompressor::create(move(upstream)));
    case ContentEncoding::Deflate:
        // TODO: Auto-detect if raw deflate should be used or zlib-wrapped. Chrome and Firefox detects this even though the RFC 7230 says it's zlib-wrapped.
        return TRY(Compress::ZlibDecompressor::create(move(upstream)));
    case ContentEncoding::Brotli:
        // TODO: Implement in LibCompress and add the decompressor here.
        return Error::from_string_literal("Brotli decompression not yet implemented");
        case ContentEncoding::Zstd:
        // TODO: Implement in LibCompress and add the decompressor here.
        return Error::from_string_literal("Zstd decompression not yet implemented");
    }
    VERIFY_NOT_REACHED();
}

NonnullOwnPtr<BodyDecoder> BodyDecoder::create_pass_through()
{
    return adopt_own(*new BodyDecoder);
}

ErrorOr<NonnullOwnPtr<BodyDecoder>> BodyDecoder::create(Vector<ContentEncoding> const& encoding_chain)
{
    VERIFY(!encoding_chain.is_empty());

    auto feeder = TRY(adopt_nonnull_own_or_enomem(new (nothrow) FeederStream));

    // Build the stream decompression pipeline in reverse header order. Per RFC 7231:
    // Content-Encoding lists the order the encodings where applied so we need to reverse it
    OwnPtr<Stream> current;
    for (auto encoding : encoding_chain.in_reverse()) {
        MaybeOwned<Stream> upstream = current
            ? MaybeOwned<Stream>(current.release_nonnull())
            : MaybeOwned<Stream>(*feeder);
        current = TRY(wrap_with_decoder(encoding, move(upstream)));
    }
    VERIFY(current);

    return adopt_nonnull_own_or_enomem(new (nothrow) BodyDecoder(move(feeder), current.release_nonnull()));
}

BodyDecoder::BodyDecoder(NonnullOwnPtr<FeederStream> feeder, NonnullOwnPtr<Stream> top)
    : m_feeder(move(feeder))
    , m_top(move(top))
{
}

BodyDecoder::~BodyDecoder() = default;

ErrorOr<void> BodyDecoder::push(ReadonlyBytes wire_bytes, AllocatingMemoryStream& out)
{
    if (is_pass_through()) {
        TRY(out.write_until_depleted(wire_bytes));
        return { };
    }

    TRY(m_feeder->write_until_depleted(wire_bytes));

    Array<u8, 4096> scratch;
    while (true) {
        auto decoded = TRY(m_top->read_some(scratch.span()));
        if (decoded.is_empty())
            break;
        TRY(out.write_until_depleted(decoded));
    }
    return { };
}

ErrorOr<void> BodyDecoder::finish(AllocatingMemoryStream& out)
{
    if (is_pass_through())
        return { };

    m_feeder->mark_eof();

    Array<u8, 4096> scratch;
    while (!m_top->is_eof()) {
        auto decoded = TRY(m_top->read_some(scratch.span()));
        if (decoded.is_empty()) {
            if (!m_top->is_eof())
                return Error::from_string_literal("Compressed body ended before end-of-stream marker");
            break;
        }
        TRY(out.write_until_depleted(decoded));
    }
    return { };
}

}
