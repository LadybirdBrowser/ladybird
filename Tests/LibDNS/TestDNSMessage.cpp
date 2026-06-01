/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibDNS/Message.h>
#include <LibTest/TestCase.h>

// A label longer than 63 octets cannot be represented in the wire format, whose length field is a 6-bit value.
// Encoding such a label used to abort the process via a VERIFY in DomainName::to_raw(); it must now fail
// gracefully. Reachable from a query for a host name that contains an over-long label.
TEST_CASE(encoding_a_domain_name_with_an_overlong_label_does_not_crash)
{
    auto domain_name = DNS::Messages::DomainName::from_string(ByteString::repeated('a', 64));

    ByteBuffer out;
    auto result = domain_name.to_raw(out);
    EXPECT(result.is_error());
}

// A label length octet whose top two bits are 0b01 or 0b10 is reserved (RFC 1035, 4.1.4); the parser used to
// accept it as an ordinary label of up to 191 octets, producing a name that could no longer be re-encoded.
// Parsing such a message must fail instead.
TEST_CASE(parsing_a_reserved_label_length_fails)
{
    // Header with QDCOUNT = 1 and all other counts 0, then a question whose name starts with a label length octet
    // of 0x40 (reserved top bits, 64 octets).
    Vector<u8> message;
    message.extend({ 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 });
    message.append(0x40);
    for (size_t i = 0; i < 64; ++i)
        message.append('a');
    message.append(0x00);
    message.extend({ 0, 1, 0, 1 }); // QTYPE = A, QCLASS = IN

    FixedMemoryStream stream { message.span() };
    auto result = DNS::Messages::Message::from_raw(stream);
    EXPECT(result.is_error());
}
