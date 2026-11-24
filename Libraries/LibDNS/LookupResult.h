/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibDNS/Export.h>
#include <LibDNS/Types.h>

namespace DNS {

// RFC 4035, 4.3. Determining Security Status of Data.
enum class SecurityStatus : u8 {
    Secure,
    Insecure,
};

class DNS_API LookupResult : public AtomicRefCounted<LookupResult> {
public:
    using Address = Variant<IPv4Address, IPv6Address>;

    struct Record {
        Messages::ResourceType type;
        u32 ttl { 0 };
        Optional<Address> address;
        // The record's data, as `dns` prints it.
        String text;
    };

    LookupResult(ByteString name, Vector<Record> records, Optional<SecurityStatus> security_status)
        : m_name(move(name))
        , m_records(move(records))
        , m_security_status(security_status)
    {
    }

    ByteString const& name() const { return m_name; }
    Vector<Record> const& records() const { return m_records; }
    bool is_empty() const { return m_records.is_empty(); }

    Vector<Address> cached_addresses() const
    {
        Vector<Address> addresses;
        for (auto const& record : m_records) {
            if (record.address.has_value())
                addresses.append(*record.address);
        }
        return addresses;
    }

    bool has_cached_addresses() const
    {
        return any_of(m_records, [](auto const& record) { return record.address.has_value(); });
    }

    // Whether DNSSEC validation was attempted, with the result being Secure or Insecure.
    bool was_dnssec_validated() const { return m_security_status.has_value(); }
    bool is_dnssec_validated() const { return m_security_status == SecurityStatus::Secure; }

private:
    ByteString m_name;
    Vector<Record> m_records;
    Optional<SecurityStatus> m_security_status;
};

}
