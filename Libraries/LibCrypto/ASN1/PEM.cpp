/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/GenericLexer.h>
#include <LibCrypto/ASN1/PEM.h>

namespace Crypto {

ByteBuffer decode_pem(ReadonlyBytes data)
{
    GenericLexer lexer { data };
    ByteBuffer decoded;

    // FIXME: Parse multiple.
    enum {
        PreStartData,
        Started,
        Ended,
    } state { PreStartData };
    while (!lexer.is_eof()) {
        switch (state) {
        case PreStartData:
            if (lexer.consume_specific("-----BEGIN"sv))
                state = Started;
            lexer.consume_line();
            break;
        case Started: {
            if (lexer.consume_specific("-----END"sv)) {
                state = Ended;
                lexer.consume_line();
                break;
            }
            auto b64decoded = decode_base64(lexer.consume_line().trim_whitespace(TrimMode::Right));
            if (b64decoded.is_error()) {
                dbgln("Failed to decode PEM: {}", b64decoded.error().string_literal());
                return {};
            }
            if (decoded.try_append(b64decoded.value().data(), b64decoded.value().size()).is_error()) {
                dbgln("Failed to decode PEM, likely OOM condition");
                return {};
            }
            break;
        }
        case Ended:
            lexer.consume_all();
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return decoded;
}

ErrorOr<Vector<ByteBuffer>> decode_pems(ReadonlyBytes data)
{
    GenericLexer lexer { data };
    ByteBuffer decoded;
    Vector<ByteBuffer> pems;

    enum {
        Junk,
        Parsing,
    } state { Junk };
    while (!lexer.is_eof()) {
        switch (state) {
        case Junk:
            if (lexer.consume_specific("-----BEGIN"sv))
                state = Parsing;
            lexer.consume_line();
            break;
        case Parsing: {
            if (lexer.consume_specific("-----END"sv)) {
                state = Junk;
                lexer.consume_line();
                TRY(pems.try_append(decoded));
                decoded.clear();
                break;
            }
            auto b64decoded = TRY(decode_base64(lexer.consume_line().trim_whitespace(TrimMode::Right)));
            TRY(decoded.try_append(b64decoded.data(), b64decoded.size()));
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return pems;
}

ErrorOr<ByteBuffer> encode_pem(ReadonlyBytes data, PEMType type)
{
    ByteBuffer encoded;
    StringView block_start;
    StringView block_end;

    switch (type) {
    case Certificate:
        block_start = "-----BEGIN CERTIFICATE-----\n"sv;
        block_end = "-----END CERTIFICATE-----\n"sv;
        break;
    case PrivateKey:
        block_start = "-----BEGIN PRIVATE KEY-----\n"sv;
        block_end = "-----END PRIVATE KEY-----\n"sv;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto b64encoded = TRY(encode_base64(data));

    TRY(encoded.try_append(block_start.bytes()));

    size_t to_read = 64;
    for (size_t i = 0; i < b64encoded.bytes().size(); i += to_read) {
        if (i + to_read > b64encoded.bytes().size())
            to_read = b64encoded.bytes().size() - i;
        TRY(encoded.try_append(b64encoded.bytes().slice(i, to_read)));
        TRY(encoded.try_append("\n"sv.bytes()));
    }

    TRY(encoded.try_append(block_end.bytes()));

    return encoded;
}

}
