/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CountingStream.h>
#include <AK/MemoryStream.h>
#include <AK/Stream.h>
#include <AK/UFixedBigInt.h>
#include <LibCore/DateTime.h>
#include <LibDNS/Message.h>

namespace DNS::Messages {

String Options::to_string() const
{
    StringBuilder builder;
    builder.appendff("QR: {}, Opcode: {}, AA: {}, TC: {}, RD: {}, RA: {}, AD: {}, CD: {}, RCODE: {}",
        is_question() ? "Q" : "R",
        Messages::to_string(op_code()),
        is_authoritative_answer(),
        is_truncated(),
        recursion_desired(),
        recursion_available(),
        authenticated_data(),
        checking_disabled(),
        Messages::to_string(response_code()));
    return MUST(builder.to_string());
}

StringView to_string(Options::ResponseCode code)
{
    switch (code) {
    case Options::ResponseCode::NoError:
        return "NoError"sv;
    case Options::ResponseCode::FormatError:
        return "FormatError"sv;
    case Options::ResponseCode::ServerFailure:
        return "ServerFailure"sv;
    case Options::ResponseCode::NameError:
        return "NameError"sv;
    case Options::ResponseCode::NotImplemented:
        return "NotImplemented"sv;
    case Options::ResponseCode::Refused:
        return "Refused"sv;
    default:
        return "UNKNOWN"sv;
    }
}

ErrorOr<Message> Message::from_raw(AK::Stream& stream)
{
    CountingStream counting_stream { MaybeOwned(stream) };
    auto context = ParseContext { counting_stream, make<RedBlackTree<u16, DomainName>>() };
    return from_raw(context);
}

ErrorOr<Message> Message::from_raw(ParseContext& ctx)
{
    // RFC 1035, 4.1. (Messages) Format.
    // | Header      |
    // | Question    | the question for the name server
    // | Answer      | RRs answering the question
    // | Authority   | RRs pointing toward an authority
    // | Additional  | RRs holding additional information
    //
    // The header section is always present.  The header includes fields that
    // specify which of the remaining sections are present, and also specify
    // whether the message is a query or a response, a standard query or some
    // other opcode, etc.

    Header header;
    Bytes header_bytes { &header, sizeof(Header) };
    TRY(ctx.stream.read_until_filled(header_bytes));

    Message message {};
    message.header = header;

    for (size_t i = 0; i < header.question_count; ++i) {
        auto question = TRY(Question::from_raw(ctx));
        message.questions.append(move(question));
    }

    for (size_t i = 0; i < header.answer_count; ++i) {
        auto answer = TRY(ResourceRecord::from_raw(ctx));
        message.answers.append(move(answer));
    }

    for (size_t i = 0; i < header.authority_count; ++i) {
        auto authority = TRY(ResourceRecord::from_raw(ctx));
        message.authorities.append(move(authority));
    }

    for (size_t i = 0; i < header.additional_count; ++i) {
        auto additional = TRY(ResourceRecord::from_raw(ctx));
        message.additional_records.append(move(additional));
    }

    return message;
}

ErrorOr<size_t> Message::to_raw(ByteBuffer& out) const
{
    // NOTE: This is minimally implemented to allow for sending queries,
    //       server-side responses are not implemented yet.
    VERIFY(header.answer_count == 0);
    VERIFY(header.authority_count == 0);

    auto start_size = out.size();

    auto header_bytes = TRY(out.get_bytes_for_writing(sizeof(Header)));
    memcpy(header_bytes.data(), &header, sizeof(Header));

    for (size_t i = 0; i < header.question_count; i++)
        TRY(questions[i].to_raw(out));

    for (size_t i = 0; i < header.additional_count; i++)
        TRY(additional_records[i].to_raw(out));

    return out.size() - start_size;
}

ErrorOr<String> Message::format_for_log() const
{
    StringBuilder builder;
    builder.appendff("ID: {}\n", header.id);
    builder.appendff("Flags: {} ({:x})\n", header.options.to_string(), header.options.raw);
    builder.appendff("qdcount: {}, ancount: {}, nscount: {}, arcount: {}\n", header.question_count, header.answer_count, header.authority_count, header.additional_count);

    if (header.question_count > 0) {
        builder.appendff("Questions:\n");
        for (auto& q : questions)
            builder.appendff("    {} {} {}\n", q.name.to_string(), to_string(q.class_), to_string(q.type));
    }

    if (header.answer_count > 0) {
        builder.appendff("Answers:\n");
        for (auto& a : answers) {
            builder.appendff("    {} {} {}\n", a.name.to_string(), to_string(a.class_), to_string(a.type));
            a.record.visit(
                [&](auto const& record) { builder.appendff("        {}\n", MUST(record.to_string())); },
                [&](ByteBuffer const& raw) {
                    builder.appendff("        {:hex-dump}\n", raw.bytes());
                });
        }
    }

    if (header.authority_count > 0) {
        builder.appendff("Authorities:\n");
        for (auto& a : authorities) {
            builder.appendff("    {} {} {}\n", a.name.to_string(), to_string(a.class_), to_string(a.type));
            a.record.visit(
                [&](auto const& record) { builder.appendff("        {}\n", MUST(record.to_string())); },
                [&](ByteBuffer const& raw) {
                    builder.appendff("        {:hex-dump}\n", raw.bytes());
                });
        }
    }

    if (header.additional_count > 0) {
        builder.appendff("Additional:\n");
        for (auto& a : additional_records) {
            builder.appendff("    {} {} {}\n", a.name.to_string(), to_string(a.type), to_string(a.class_));
            a.record.visit(
                [&](auto const& record) { builder.appendff("        {}\n", MUST(record.to_string())); },
                [&](ByteBuffer const& raw) {
                    builder.appendff("        {:hex-dump}\n", raw.bytes());
                });
        }
    }

    return builder.to_string();
}

ErrorOr<Question> Question::from_raw(ParseContext& ctx)
{
    // RFC 1035, 4.1.2. Question section format.
    // +        +
    // | QNAME  | a domain name represented as a sequence of labels
    // +        +
    // | QTYPE  | a two octet code which specifies the type of the query
    // | QCLASS | a two octet code that specifies the class of the query

    auto name = TRY(DomainName::from_raw(ctx));
    auto type = static_cast<ResourceType>(static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>())));
    auto class_ = static_cast<Class>(static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>())));

    return Question { move(name), type, class_ };
}

ErrorOr<void> Question::to_raw(ByteBuffer& out) const
{
    TRY(name.to_raw(out));

    auto type_bytes = TRY(out.get_bytes_for_writing(2));
    auto net_type = static_cast<NetworkOrdered<u16>>(to_underlying(type));
    memcpy(type_bytes.data(), &net_type, 2);

    auto class_bytes = TRY(out.get_bytes_for_writing(2));
    auto net_class = static_cast<NetworkOrdered<u16>>(to_underlying(class_));
    memcpy(class_bytes.data(), &net_class, 2);

    return {};
}

StringView to_string(ResourceType type)
{
    switch (type) {
    case ResourceType::Reserved:
        return "Reserved"sv;
    case ResourceType::A:
        return "A"sv;
    case ResourceType::NS:
        return "NS"sv;
    case ResourceType::MD:
        return "MD"sv;
    case ResourceType::MF:
        return "MF"sv;
    case ResourceType::CNAME:
        return "CNAME"sv;
    case ResourceType::SOA:
        return "SOA"sv;
    case ResourceType::MB:
        return "MB"sv;
    case ResourceType::MG:
        return "MG"sv;
    case ResourceType::MR:
        return "MR"sv;
    case ResourceType::NULL_:
        return "NULL_"sv;
    case ResourceType::WKS:
        return "WKS"sv;
    case ResourceType::PTR:
        return "PTR"sv;
    case ResourceType::HINFO:
        return "HINFO"sv;
    case ResourceType::MINFO:
        return "MINFO"sv;
    case ResourceType::MX:
        return "MX"sv;
    case ResourceType::TXT:
        return "TXT"sv;
    case ResourceType::RP:
        return "RP"sv;
    case ResourceType::AFSDB:
        return "AFSDB"sv;
    case ResourceType::X25:
        return "X25"sv;
    case ResourceType::ISDN:
        return "ISDN"sv;
    case ResourceType::RT:
        return "RT"sv;
    case ResourceType::NSAP:
        return "NSAP"sv;
    case ResourceType::NSAP_PTR:
        return "NSAP_PTR"sv;
    case ResourceType::SIG:
        return "SIG"sv;
    case ResourceType::KEY:
        return "KEY"sv;
    case ResourceType::PX:
        return "PX"sv;
    case ResourceType::GPOS:
        return "GPOS"sv;
    case ResourceType::AAAA:
        return "AAAA"sv;
    case ResourceType::LOC:
        return "LOC"sv;
    case ResourceType::NXT:
        return "NXT"sv;
    case ResourceType::EID:
        return "EID"sv;
    case ResourceType::NIMLOC:
        return "NIMLOC"sv;
    case ResourceType::SRV:
        return "SRV"sv;
    case ResourceType::ATMA:
        return "ATMA"sv;
    case ResourceType::NAPTR:
        return "NAPTR"sv;
    case ResourceType::KX:
        return "KX"sv;
    case ResourceType::CERT:
        return "CERT"sv;
    case ResourceType::A6:
        return "A6"sv;
    case ResourceType::DNAME:
        return "DNAME"sv;
    case ResourceType::SINK:
        return "SINK"sv;
    case ResourceType::OPT:
        return "OPT"sv;
    case ResourceType::APL:
        return "APL"sv;
    case ResourceType::DS:
        return "DS"sv;
    case ResourceType::SSHFP:
        return "SSHFP"sv;
    case ResourceType::IPSECKEY:
        return "IPSECKEY"sv;
    case ResourceType::RRSIG:
        return "RRSIG"sv;
    case ResourceType::NSEC:
        return "NSEC"sv;
    case ResourceType::DNSKEY:
        return "DNSKEY"sv;
    case ResourceType::DHCID:
        return "DHCID"sv;
    case ResourceType::NSEC3:
        return "NSEC3"sv;
    case ResourceType::NSEC3PARAM:
        return "NSEC3PARAM"sv;
    case ResourceType::TLSA:
        return "TLSA"sv;
    case ResourceType::SMIMEA:
        return "SMIMEA"sv;
    case ResourceType::HIP:
        return "HIP"sv;
    case ResourceType::NINFO:
        return "NINFO"sv;
    case ResourceType::RKEY:
        return "RKEY"sv;
    case ResourceType::TALINK:
        return "TALINK"sv;
    case ResourceType::CDS:
        return "CDS"sv;
    case ResourceType::CDNSKEY:
        return "CDNSKEY"sv;
    case ResourceType::OPENPGPKEY:
        return "OPENPGPKEY"sv;
    case ResourceType::CSYNC:
        return "CSYNC"sv;
    case ResourceType::ZONEMD:
        return "ZONEMD"sv;
    case ResourceType::SVCB:
        return "SVCB"sv;
    case ResourceType::HTTPS:
        return "HTTPS"sv;
    case ResourceType::SPF:
        return "SPF"sv;
    case ResourceType::UINFO:
        return "UINFO"sv;
    case ResourceType::UID:
        return "UID"sv;
    case ResourceType::GID:
        return "GID"sv;
    case ResourceType::UNSPEC:
        return "UNSPEC"sv;
    case ResourceType::NID:
        return "NID"sv;
    case ResourceType::L32:
        return "L32"sv;
    case ResourceType::L64:
        return "L64"sv;
    case ResourceType::LP:
        return "LP"sv;
    case ResourceType::EUI48:
        return "EUI48"sv;
    case ResourceType::EUI64:
        return "EUI64"sv;
    case ResourceType::NXNAME:
        return "NXNAME"sv;
    case ResourceType::TKEY:
        return "TKEY"sv;
    case ResourceType::TSIG:
        return "TSIG"sv;
    case ResourceType::IXFR:
        return "IXFR"sv;
    case ResourceType::AXFR:
        return "AXFR"sv;
    case ResourceType::MAILB:
        return "MAILB"sv;
    case ResourceType::MAILA:
        return "MAILA"sv;
    case ResourceType::ANY:
        return "ANY"sv;
    case ResourceType::URI:
        return "URI"sv;
    case ResourceType::CAA:
        return "CAA"sv;
    case ResourceType::AVC:
        return "AVC"sv;
    case ResourceType::DOA:
        return "DOA"sv;
    case ResourceType::AMTRELAY:
        return "AMTRELAY"sv;
    case ResourceType::RESINFO:
        return "RESINFO"sv;
    case ResourceType::WALLET:
        return "WALLET"sv;
    case ResourceType::CLA:
        return "CLA"sv;
    case ResourceType::IPN:
        return "IPN"sv;
    case ResourceType::TA:
        return "TA"sv;
    case ResourceType::DLV:
        return "DLV"sv;
    default:
        return "UNKNOWN"sv;
    }
}

Optional<ResourceType> resource_type_from_string(StringView name)
{
    if (name == "Reserved"sv)
        return ResourceType::Reserved;
    if (name == "A"sv)
        return ResourceType::A;
    if (name == "NS"sv)
        return ResourceType::NS;
    if (name == "MD"sv)
        return ResourceType::MD;
    if (name == "MF"sv)
        return ResourceType::MF;
    if (name == "CNAME"sv)
        return ResourceType::CNAME;
    if (name == "SOA"sv)
        return ResourceType::SOA;
    if (name == "MB"sv)
        return ResourceType::MB;
    if (name == "MG"sv)
        return ResourceType::MG;
    if (name == "MR"sv)
        return ResourceType::MR;
    if (name == "NULL_"sv)
        return ResourceType::NULL_;
    if (name == "WKS"sv)
        return ResourceType::WKS;
    if (name == "PTR"sv)
        return ResourceType::PTR;
    if (name == "HINFO"sv)
        return ResourceType::HINFO;
    if (name == "MINFO"sv)
        return ResourceType::MINFO;
    if (name == "MX"sv)
        return ResourceType::MX;
    if (name == "TXT"sv)
        return ResourceType::TXT;
    if (name == "RP"sv)
        return ResourceType::RP;
    if (name == "AFSDB"sv)
        return ResourceType::AFSDB;
    if (name == "X25"sv)
        return ResourceType::X25;
    if (name == "ISDN"sv)
        return ResourceType::ISDN;
    if (name == "RT"sv)
        return ResourceType::RT;
    if (name == "NSAP"sv)
        return ResourceType::NSAP;
    if (name == "NSAP_PTR"sv)
        return ResourceType::NSAP_PTR;
    if (name == "SIG"sv)
        return ResourceType::SIG;
    if (name == "KEY"sv)
        return ResourceType::KEY;
    if (name == "PX"sv)
        return ResourceType::PX;
    if (name == "GPOS"sv)
        return ResourceType::GPOS;
    if (name == "AAAA"sv)
        return ResourceType::AAAA;
    if (name == "LOC"sv)
        return ResourceType::LOC;
    if (name == "NXT"sv)
        return ResourceType::NXT;
    if (name == "EID"sv)
        return ResourceType::EID;
    if (name == "NIMLOC"sv)
        return ResourceType::NIMLOC;
    if (name == "SRV"sv)
        return ResourceType::SRV;
    if (name == "ATMA"sv)
        return ResourceType::ATMA;
    if (name == "NAPTR"sv)
        return ResourceType::NAPTR;
    if (name == "KX"sv)
        return ResourceType::KX;
    if (name == "CERT"sv)
        return ResourceType::CERT;
    if (name == "A6"sv)
        return ResourceType::A6;
    if (name == "DNAME"sv)
        return ResourceType::DNAME;
    if (name == "SINK"sv)
        return ResourceType::SINK;
    if (name == "OPT"sv)
        return ResourceType::OPT;
    if (name == "APL"sv)
        return ResourceType::APL;
    if (name == "DS"sv)
        return ResourceType::DS;
    if (name == "SSHFP"sv)
        return ResourceType::SSHFP;
    if (name == "IPSECKEY"sv)
        return ResourceType::IPSECKEY;
    if (name == "RRSIG"sv)
        return ResourceType::RRSIG;
    if (name == "NSEC"sv)
        return ResourceType::NSEC;
    if (name == "DNSKEY"sv)
        return ResourceType::DNSKEY;
    if (name == "DHCID"sv)
        return ResourceType::DHCID;
    if (name == "NSEC3"sv)
        return ResourceType::NSEC3;
    if (name == "NSEC3PARAM"sv)
        return ResourceType::NSEC3PARAM;
    if (name == "TLSA"sv)
        return ResourceType::TLSA;
    if (name == "SMIMEA"sv)
        return ResourceType::SMIMEA;
    if (name == "HIP"sv)
        return ResourceType::HIP;
    if (name == "NINFO"sv)
        return ResourceType::NINFO;
    if (name == "RKEY"sv)
        return ResourceType::RKEY;
    if (name == "TALINK"sv)
        return ResourceType::TALINK;
    if (name == "CDS"sv)
        return ResourceType::CDS;
    if (name == "CDNSKEY"sv)
        return ResourceType::CDNSKEY;
    if (name == "OPENPGPKEY"sv)
        return ResourceType::OPENPGPKEY;
    if (name == "CSYNC"sv)
        return ResourceType::CSYNC;
    if (name == "ZONEMD"sv)
        return ResourceType::ZONEMD;
    if (name == "SVCB"sv)
        return ResourceType::SVCB;
    if (name == "HTTPS"sv)
        return ResourceType::HTTPS;
    if (name == "SPF"sv)
        return ResourceType::SPF;
    if (name == "UINFO"sv)
        return ResourceType::UINFO;
    if (name == "UID"sv)
        return ResourceType::UID;
    if (name == "GID"sv)
        return ResourceType::GID;
    if (name == "UNSPEC"sv)
        return ResourceType::UNSPEC;
    if (name == "NID"sv)
        return ResourceType::NID;
    if (name == "L32"sv)
        return ResourceType::L32;
    if (name == "L64"sv)
        return ResourceType::L64;
    if (name == "LP"sv)
        return ResourceType::LP;
    if (name == "EUI48"sv)
        return ResourceType::EUI48;
    if (name == "EUI64"sv)
        return ResourceType::EUI64;
    if (name == "NXNAME"sv)
        return ResourceType::NXNAME;
    if (name == "TKEY"sv)
        return ResourceType::TKEY;
    if (name == "TSIG"sv)
        return ResourceType::TSIG;
    if (name == "IXFR"sv)
        return ResourceType::IXFR;
    if (name == "AXFR"sv)
        return ResourceType::AXFR;
    if (name == "MAILB"sv)
        return ResourceType::MAILB;
    if (name == "MAILA"sv)
        return ResourceType::MAILA;
    if (name == "ANY"sv)
        return ResourceType::ANY;
    if (name == "URI"sv)
        return ResourceType::URI;
    if (name == "CAA"sv)
        return ResourceType::CAA;
    if (name == "AVC"sv)
        return ResourceType::AVC;
    if (name == "DOA"sv)
        return ResourceType::DOA;
    if (name == "AMTRELAY"sv)
        return ResourceType::AMTRELAY;
    if (name == "RESINFO"sv)
        return ResourceType::RESINFO;
    if (name == "WALLET"sv)
        return ResourceType::WALLET;
    if (name == "CLA"sv)
        return ResourceType::CLA;
    if (name == "IPN"sv)
        return ResourceType::IPN;
    if (name == "TA"sv)
        return ResourceType::TA;
    if (name == "DLV"sv)
        return ResourceType::DLV;
    return {};
}

StringView to_string(Class class_)
{
    switch (class_) {
    case Class::IN:
        return "IN"sv;
    case Class::CH:
        return "CH"sv;
    case Class::HS:
        return "HS"sv;
    default:
        return "UNKNOWN"sv;
    }
}

StringView to_string(OpCode code)
{
    if ((to_underlying(code) & to_underlying(OpCode::Reserved)) != 0)
        return "Reserved"sv;

    switch (code) {
    case OpCode::Query:
        return "Query"sv;
    case OpCode::IQuery:
        return "IQuery"sv;
    case OpCode::Status:
        return "Status"sv;
    case OpCode::Notify:
        return "Notify"sv;
    case OpCode::Update:
        return "Update"sv;
    case OpCode::DSO:
        return "DSO"sv;
    default:
        return "UNKNOWN"sv;
    }
}

DomainName DomainName::from_string(StringView name)
{
    DomainName domain_name;
    name.for_each_split_view('.', SplitBehavior::Nothing, [&](StringView piece) {
        domain_name.labels.append(piece);
    });
    return domain_name;
}

ErrorOr<DomainName> DomainName::from_raw(ParseContext& ctx)
{
    // RFC 1035, 4.1.2. Question section format.
    // QNAME           a domain name represented as a sequence of labels, where
    //                each label consists of a length octet followed by that
    //                number of octets.  The domain name terminates with the
    //                zero length octet for the null label of the root.  Note
    //                that this field may be an odd number of octets; no
    //                padding is used.
    DomainName name;
    auto input_offset_marker = ctx.stream.read_bytes();
    while (true) {
        auto length = TRY(ctx.stream.read_value<u8>());
        if (length == 0)
            break;

        constexpr static u8 OffsetMarkerMask = 0b11000000;
        if ((length & OffsetMarkerMask) == OffsetMarkerMask) {
            // This is a pointer to a prior domain name.
            u16 const offset = static_cast<u16>(length & ~OffsetMarkerMask) << 8 | TRY(ctx.stream.read_value<u8>());
            if (auto it = ctx.pointers->find_largest_not_above_iterator(offset); !it.is_end()) {
                auto labels = it->labels;
                for (auto& entry : labels)
                    name.labels.append(entry);
                break;
            }
            dbgln("Invalid domain name pointer in label, no prior domain name found around offset {}", offset);
            return Error::from_string_literal("Invalid domain name pointer in label");
        }

        ByteBuffer content;
        TRY(ctx.stream.read_until_filled(TRY(content.get_bytes_for_writing(length))));
        name.labels.append(ByteString::copy(content));
    }

    ctx.pointers->insert(input_offset_marker, name);

    return name;
}

ErrorOr<void> DomainName::to_raw(ByteBuffer& out) const
{
    for (auto& label : labels) {
        VERIFY(label.length() <= 63);
        auto size_bytes = TRY(out.get_bytes_for_writing(1));
        u8 size = static_cast<u8>(label.length());
        memcpy(size_bytes.data(), &size, 1);

        auto content_bytes = TRY(out.get_bytes_for_writing(label.length()));
        memcpy(content_bytes.data(), label.characters(), label.length());
    }

    TRY(out.try_append(0));

    return {};
}

String DomainName::to_string() const
{
    StringBuilder builder;
    for (size_t i = 0; i < labels.size(); ++i) {
        builder.append(labels[i]);
        builder.append('.');
    }

    return MUST(builder.to_string());
}

class RecordingStream final : public Stream {
public:
    explicit RecordingStream(Stream& stream)
        : m_stream(stream)
    {
    }

    ByteBuffer take_recorded_data() && { return move(m_recorded_data); }

    virtual ErrorOr<Bytes> read_some(Bytes bytes) override
    {
        auto result = TRY(m_stream->read_some(bytes));
        m_recorded_data.append(result.data(), result.size());
        return result;
    }
    virtual ErrorOr<void> discard(size_t discarded_bytes) override
    {
        auto space = TRY(m_recorded_data.get_bytes_for_writing(discarded_bytes));
        TRY(m_stream->read_until_filled(space));
        return {};
    }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes bytes) override { return m_stream->write_some(bytes); }
    virtual bool is_eof() const override { return m_stream->is_eof(); }
    virtual bool is_open() const override { return m_stream->is_open(); }
    virtual void close() override { m_stream->close(); }

private:
    MaybeOwned<Stream> m_stream;
    ByteBuffer m_recorded_data;
};

ErrorOr<ResourceRecord> ResourceRecord::from_raw(ParseContext& ctx)
{
    // RFC 1035, 4.1.3. Resource record format.
    // +           +
    // | NAME      | a domain name to which this resource record pertains
    // +           +
    // | TYPE      | two octets containing one of the RR type codes
    // | CLASS     | two octets containing one of the RR class codes
    // | TTL       | a 32-bit unsigned integer that specifies the time interval
    // |           | that the resource record may be cached
    // | RDLENGTH  | an unsigned 16-bit integer that specifies the length in
    // |           | octets of the RDATA field
    // | RDATA     | a variable length string of octets that describes the resource

    ByteBuffer rdata;
    ByteBuffer rr_raw_data;
    DomainName name;
    ResourceType type;
    Class class_;
    u32 ttl;
    {
        RecordingStream rr_stream { ctx.stream };
        CountingStream rr_counting_stream { MaybeOwned<Stream>(rr_stream) };
        ParseContext rr_ctx { rr_counting_stream, move(ctx.pointers) };
        ScopeGuard guard([&] { ctx.pointers = move(rr_ctx.pointers); });

        name = TRY(DomainName::from_raw(rr_ctx));
        type = static_cast<ResourceType>(static_cast<u16>(TRY(rr_ctx.stream.read_value<NetworkOrdered<u16>>())));
        if (type == ResourceType::OPT) {
            auto record = ResourceRecord {
                move(name),
                type,
                Class::IN,
                0,
                TRY(Records::OPT::from_raw(rr_ctx)),
                {},
            };
            record.raw = move(rr_stream).take_recorded_data();
            return record;
        }
        class_ = static_cast<Class>(static_cast<u16>(TRY(rr_ctx.stream.read_value<NetworkOrdered<u16>>())));
        ttl = static_cast<u32>(TRY(rr_ctx.stream.read_value<NetworkOrdered<u32>>()));
        auto rd_length = static_cast<u16>(TRY(rr_ctx.stream.read_value<NetworkOrdered<u16>>()));
        TRY(rr_ctx.stream.read_until_filled(TRY(rdata.get_bytes_for_writing(rd_length))));

        rr_raw_data = move(rr_stream).take_recorded_data();
    }

    FixedMemoryStream stream { rdata.bytes() };
    CountingStream rdata_stream { MaybeOwned<Stream>(stream) };
    ParseContext rdata_ctx { rdata_stream, move(ctx.pointers) };
    ScopeGuard guard([&] { ctx.pointers = move(rdata_ctx.pointers); });

#define PARSE_AS_RR(TYPE)                                                                                                                                   \
    do {                                                                                                                                                    \
        auto rr = TRY(Records::TYPE::from_raw(rdata_ctx));                                                                                                  \
        if (!rdata_stream.is_eof()) {                                                                                                                       \
            dbgln("Extra data ({}) left in stream: {:hex-dump}", rdata.size() - rdata_stream.read_bytes(), rdata.bytes().slice(rdata_stream.read_bytes())); \
            return Error::from_string_literal("Extra data in " #TYPE " record content");                                                                    \
        }                                                                                                                                                   \
        return ResourceRecord { move(name), type, class_, ttl, rr, move(rr_raw_data) };                                                                     \
    } while (0)

    switch (type) {
    case ResourceType::A:
        PARSE_AS_RR(A);
    case ResourceType::AAAA:
        PARSE_AS_RR(AAAA);
    case ResourceType::TXT:
        PARSE_AS_RR(TXT);
    case ResourceType::CNAME:
        PARSE_AS_RR(CNAME);
    case ResourceType::NS:
        PARSE_AS_RR(NS);
    case ResourceType::SOA:
        PARSE_AS_RR(SOA);
    case ResourceType::MX:
        PARSE_AS_RR(MX);
    case ResourceType::PTR:
        PARSE_AS_RR(PTR);
    case ResourceType::SRV:
        PARSE_AS_RR(SRV);
    case ResourceType::DNSKEY:
        PARSE_AS_RR(DNSKEY);
    case ResourceType::CDNSKEY:
        PARSE_AS_RR(CDNSKEY);
    case ResourceType::DS:
        PARSE_AS_RR(DS);
    case ResourceType::CDS:
        PARSE_AS_RR(CDS);
    case ResourceType::RRSIG:
        PARSE_AS_RR(RRSIG);
    // case ResourceType::NSEC:
    //     PARSE_AS_RR(NSEC);
    // case ResourceType::NSEC3:
    //     PARSE_AS_RR(NSEC3);
    // case ResourceType::NSEC3PARAM:
    //     PARSE_AS_RR(NSEC3PARAM);
    // case ResourceType::TLSA:
    //     PARSE_AS_RR(TLSA);
    case ResourceType::HINFO:
        PARSE_AS_RR(HINFO);
    default:
        return ResourceRecord { move(name), type, class_, ttl, move(rdata), move(rr_raw_data) };
    }
#undef PARSE_AS_RR
}

ErrorOr<void> ResourceRecord::to_raw(ByteBuffer& buffer) const
{
    TRY(name.to_raw(buffer));

    auto type_bytes = TRY(buffer.get_bytes_for_writing(2));
    auto net_type = static_cast<NetworkOrdered<u16>>(to_underlying(type));
    memcpy(type_bytes.data(), &net_type, 2);

    if (type != ResourceType::OPT) {
        auto class_bytes = TRY(buffer.get_bytes_for_writing(2));
        auto net_class = static_cast<NetworkOrdered<u16>>(to_underlying(class_));
        memcpy(class_bytes.data(), &net_class, 2);

        auto ttl_bytes = TRY(buffer.get_bytes_for_writing(4));
        auto net_ttl = static_cast<NetworkOrdered<u32>>(ttl);
        memcpy(ttl_bytes.data(), &net_ttl, 4);
    }

    ByteBuffer rdata;
    TRY(record.visit(
        [&](auto const& record) { return record.to_raw(rdata); },
        [&](ByteBuffer const& raw) { return rdata.try_append(raw); }));

    if (type != ResourceType::OPT) {
        auto rdata_length_bytes = TRY(buffer.get_bytes_for_writing(2));
        auto net_rdata_length = static_cast<NetworkOrdered<u16>>(rdata.size());
        memcpy(rdata_length_bytes.data(), &net_rdata_length, 2);
    }

    TRY(buffer.try_append(rdata));

    return {};
}

ErrorOr<String> ResourceRecord::to_string() const
{
    StringBuilder builder;
    record.visit(
        [&](auto const& record) { builder.appendff("{}", MUST(record.to_string())); },
        [&](ByteBuffer const& raw) { builder.appendff("{:hex-dump}", raw.bytes()); });
    return builder.to_string();
}

ErrorOr<Records::A> Records::A::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.4.1. A RDATA format.
    // | ADDRESS | a 32 bit Internet address.

    u32 const address = TRY(ctx.stream.read_value<LittleEndian<u32>>());
    return Records::A { IPv4Address { address } };
}

ErrorOr<Records::AAAA> Records::AAAA::from_raw(ParseContext& ctx)
{
    // RFC 3596, 2.2. AAAA RDATA format.
    // | ADDRESS | a 128 bit Internet address.

    u128 const address = TRY(ctx.stream.read_value<LittleEndian<u128>>());
    return Records::AAAA { IPv6Address { bit_cast<Array<u8, 16>>(address) } };
}

ErrorOr<Records::TXT> Records::TXT::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.14. TXT RDATA format.
    // | TXT-DATA | a <character-string> which is used for human readability.

    auto length = TRY(ctx.stream.read_value<u8>());
    ByteBuffer content;
    TRY(ctx.stream.read_until_filled(TRY(content.get_bytes_for_writing(length))));
    return Records::TXT { ByteString::copy(content) };
}

ErrorOr<Records::CNAME> Records::CNAME::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.1. CNAME RDATA format.
    // | CNAME | a <domain-name> which specifies the canonical or primary name for the owner.

    auto name = TRY(DomainName::from_raw(ctx));
    return Records::CNAME { move(name) };
}

ErrorOr<Records::NS> Records::NS::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.11. NS RDATA format.
    // | NSDNAME | a <domain-name> which specifies a host which should be authoritative for the specified class and domain.

    auto name = TRY(DomainName::from_raw(ctx));
    return Records::NS { move(name) };
}

ErrorOr<Records::SOA> Records::SOA::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.13. SOA RDATA format.
    // | MNAME   | <domain-name> which specifies the name of the host where the master file for the zone is maintained.
    // | RNAME   | <domain-name> which specifies the mailbox of the person responsible for this zone.
    // | SERIAL  | a 32-bit unsigned integer that specifies the version number of the original copy of the zone.
    // | REFRESH | a 32-bit unsigned integer that specifies the time interval before the zone should be refreshed.
    // | RETRY   | a 32-bit unsigned integer that specifies the time interval that should elapse before a failed refresh should be retried.
    // | EXPIRE  | a 32-bit unsigned integer that specifies the time value that specifies the upper limit on the time interval that can elapse before the zone is no longer authoritative.
    // | MINIMUM | a 32-bit unsigned integer that specifies the minimum TTL field that should be exported with any RR from this zone.

    auto mname = TRY(DomainName::from_raw(ctx));
    auto rname = TRY(DomainName::from_raw(ctx));
    auto serial = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto refresh = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto retry = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto expire = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto minimum = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));

    return Records::SOA { move(mname), move(rname), serial, refresh, retry, expire, minimum };
}

ErrorOr<Records::MX> Records::MX::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.9. MX RDATA format.
    // | PREFERENCE | a 16 bit integer which specifies the preference given to this RR among others at the same owner.
    // | EXCHANGE   | a <domain-name> which specifies a host willing to act as a mail exchange for the owner name.

    auto preference = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto exchange = TRY(DomainName::from_raw(ctx));
    return Records::MX { preference, move(exchange) };
}

ErrorOr<Records::PTR> Records::PTR::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.12. PTR RDATA format.
    // | PTRDNAME | a <domain-name> which points to some location in the domain name space.

    auto name = TRY(DomainName::from_raw(ctx));
    return Records::PTR { move(name) };
}

ErrorOr<Records::SRV> Records::SRV::from_raw(ParseContext& ctx)
{
    // RFC 2782, 2. Service location and priority.
    // | PRIORITY | a 16 bit integer that specifies the priority of this target host.
    // | WEIGHT   | a 16 bit integer that specifies a weight for this target host.
    // | PORT     | a 16 bit integer that specifies the port on this target host.
    // | TARGET   | a <domain-name> which specifies the target host.

    auto priority = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto weight = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto port = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto target = TRY(DomainName::from_raw(ctx));
    return Records::SRV { priority, weight, port, move(target) };
}

ErrorOr<Records::DNSKEY> Records::DNSKEY::from_raw(ParseContext& ctx)
{
    // RFC 4034, 2.1. The DNSKEY Resource Record.
    // | FLAGS    | a 16-bit value that flags the key.
    // | PROTOCOL | an 8-bit value that specifies the protocol for this key.
    // | ALGORITHM| an 8-bit value that identifies the public key's cryptographic algorithm.
    // | PUBLICKEY| the public key material.

    auto flags = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto protocol = TRY(ctx.stream.read_value<u8>());
    auto algorithm = static_cast<DNSSEC::Algorithm>(static_cast<u8>(TRY(ctx.stream.read_value<u8>())));
    auto public_key = TRY(ctx.stream.read_until_eof());
    return Records::DNSKEY { flags, protocol, algorithm, move(public_key) };
}

ErrorOr<Records::DS> Records::DS::from_raw(ParseContext& ctx)
{
    // RFC 4034, 5.1. The DS Resource Record.
    // | KEYTAG      | a 16-bit value that identifies the DNSKEY RR.
    // | ALGORITHM   | an 8-bit value that identifies the DS's hash algorithm.
    // | DIGEST TYPE | an 8-bit value that identifies the DS's digest algorithm.
    // | DIGEST      | Digest of the DNSKEY RDATA (Flags | Protocol | Algorithm | Pubkey).

    auto key_tag = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto algorithm = static_cast<DNSSEC::Algorithm>(static_cast<u8>(TRY(ctx.stream.read_value<u8>())));
    auto digest_type = static_cast<DNSSEC::DigestType>(static_cast<u8>(TRY(ctx.stream.read_value<u8>())));
    size_t digest_size;
    switch (digest_type) {
    case DNSSEC::DigestType::SHA1:
        digest_size = 20;
        break;
    case DNSSEC::DigestType::SHA256:
    case DNSSEC::DigestType::GOST3411:
        digest_size = 32;
        break;
    case DNSSEC::DigestType::SHA384:
        digest_size = 48;
        break;
    case DNSSEC::DigestType::SHA512:
        digest_size = 64;
        break;
    case DNSSEC::DigestType::SHA224:
        digest_size = 28;
        break;
    case DNSSEC::DigestType::Unknown:
    default:
        return Error::from_string_literal("Unknown digest type in DS record");
    }

    ByteBuffer digest;
    TRY(ctx.stream.read_until_filled(TRY(digest.get_bytes_for_writing(digest_size))));
    return Records::DS { key_tag, algorithm, digest_type, move(digest) };
}

ErrorOr<Records::SIG> Records::SIG::from_raw(ParseContext& ctx)
{
    // RFC 4034, 2.2. The SIG Resource Record.
    // | TYPE-COVERED | a 16-bit value that specifies the type of the RRset that is covered by this SIG.
    // | ALGORITHM    | an 8-bit value that specifies the algorithm used to create the signature.
    // | LABELS       | an 8-bit value that specifies the number of labels in the original SIG RR owner name.
    // | ORIGINAL TTL | a 32-bit value that specifies the TTL of the covered RRset as it appears in the authoritative zone.
    // | SIGNATURE EXPIRATION | a 32-bit value that specifies the expiration date of the signature.
    // | SIGNATURE INCEPTION  | a 32-bit value that specifies the inception date of the signature.
    // | KEY TAG      | a 16-bit value that contains the key tag value of the DNSKEY RR that was used to create the signature.
    // | SIGNER'S NAME| a <domain-name> which specifies the domain name of the signer generating the SIG RR.
    // | SIGNATURE    | a <signature> that authenticates the RRs.

    auto type_covered = static_cast<ResourceType>(static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>())));
    auto algorithm = static_cast<DNSSEC::Algorithm>(static_cast<u8>(TRY(ctx.stream.read_value<u8>())));
    auto labels = TRY(ctx.stream.read_value<u8>());
    auto original_ttl = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto signature_expiration = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto signature_inception = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto key_tag = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto signer_name = TRY(DomainName::from_raw(ctx));
    auto signature = TRY(ctx.stream.read_until_eof());

    return Records::SIG { type_covered, algorithm, labels, original_ttl, UnixDateTime::from_seconds_since_epoch(signature_expiration), UnixDateTime::from_seconds_since_epoch(signature_inception), key_tag, move(signer_name), move(signature) };
}

ErrorOr<String> Records::SIG::to_string() const
{
    // Single line:
    // SIG Type covered: <type>, Algorithm: <algorithm>, Labels: <labels>, Original TTL: <ttl>, Signature expiration: <expiration>, Signature inception: <inception>, Key tag: <key tag>, Signer's name: <signer>, Signature: <signature>
    StringBuilder builder;
    builder.append("SIG "sv);
    builder.appendff("Type covered: {}, ", Messages::to_string(type_covered));
    builder.appendff("Algorithm: {}, ", DNSSEC::to_string(algorithm));
    builder.appendff("Labels: {}, ", label_count);
    builder.appendff("Original TTL: {}, ", original_ttl);
    builder.appendff("Signature expiration: {}, ", Core::DateTime::from_timestamp(expiration.truncated_seconds_since_epoch()));
    builder.appendff("Signature inception: {}, ", Core::DateTime::from_timestamp(inception.truncated_seconds_since_epoch()));
    builder.appendff("Key tag: {}, ", key_tag);
    builder.appendff("Signer's name: '{}', ", signers_name.to_string());
    builder.appendff("Signature: {}", TRY(encode_base64(signature)));
    return builder.to_string();
}

ErrorOr<Records::HINFO> Records::HINFO::from_raw(ParseContext& ctx)
{
    // RFC 1035, 3.3.2. HINFO RDATA format.
    // | CPU    | a <character-string> which specifies the CPU type.
    // | OS     | a <character-string> which specifies the operating system type.

    auto cpu_length = TRY(ctx.stream.read_value<u8>());
    ByteBuffer cpu;
    TRY(ctx.stream.read_until_filled(TRY(cpu.get_bytes_for_writing(cpu_length))));
    auto os_length = TRY(ctx.stream.read_value<u8>());
    ByteBuffer os;
    TRY(ctx.stream.read_until_filled(TRY(os.get_bytes_for_writing(os_length))));
    return Records::HINFO { ByteString::copy(cpu), ByteString::copy(os) };
}

ErrorOr<Records::OPT> Records::OPT::from_raw(ParseContext& ctx)
{
    // RFC 6891, 6.1. The OPT pseudo-RR.
    // This RR does *not* use the standard RDATA format, `ctx` starts right after 'TYPE'.
    // | NAME       | empty (root domain)
    // | TYPE       | OPT (41)
    // - we are here -
    // | UDP SIZE   | 16-bit max UDP payload size
    // | RCODE_AND_FLAGS | 32-bit flags and response code
    // | RDLENGTH   | 16-bit length of the RDATA field
    // | RDATA      | variable length, pairs of OPTION-CODE and OPTION-DATA { length(16), data(length) }

    auto udp_size = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    auto rcode_and_flags = static_cast<u32>(TRY(ctx.stream.read_value<NetworkOrdered<u32>>()));
    auto rd_length = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
    Vector<OPT::Option> options;
    while (rd_length > 0 && !ctx.stream.is_eof()) {
        auto option_code = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
        auto option_length = static_cast<u16>(TRY(ctx.stream.read_value<NetworkOrdered<u16>>()));
        ByteBuffer option_data;
        TRY(ctx.stream.read_until_filled(TRY(option_data.get_bytes_for_writing(option_length))));
        rd_length -= 4 + option_length;
        options.append(OPT::Option { option_code, move(option_data) });
    }

    if (rd_length != 0)
        return Error::from_string_literal("Invalid OPT record");

    return Records::OPT { udp_size, rcode_and_flags, move(options) };
}

ErrorOr<void> Records::OPT::to_raw(ByteBuffer& buffer) const
{
    auto udp_size_bytes = TRY(buffer.get_bytes_for_writing(sizeof(udp_payload_size)));
    auto net_udp_size = static_cast<NetworkOrdered<u16>>(udp_payload_size);
    memcpy(udp_size_bytes.data(), &net_udp_size, 2);

    auto rcode_and_flags_bytes = TRY(buffer.get_bytes_for_writing(sizeof(extended_rcode_and_flags)));
    auto net_rcode_and_flags = static_cast<NetworkOrdered<u32>>(extended_rcode_and_flags);
    memcpy(rcode_and_flags_bytes.data(), &net_rcode_and_flags, 4);

    auto rd_length_bytes = TRY(buffer.get_bytes_for_writing(2));
    u16 rd_length = 0;
    for (auto const& option : options) {
        rd_length += 4 + option.data.size();
    }
    auto net_rd_length = static_cast<NetworkOrdered<u16>>(rd_length);
    memcpy(rd_length_bytes.data(), &net_rd_length, 2);

    for (auto& option : options) {
        auto option_code_bytes = TRY(buffer.get_bytes_for_writing(sizeof(option.code)));
        auto net_option_code = static_cast<NetworkOrdered<u16>>(option.code);
        memcpy(option_code_bytes.data(), &net_option_code, 2);

        auto option_length_bytes = TRY(buffer.get_bytes_for_writing(2));
        auto net_option_length = static_cast<NetworkOrdered<u16>>(option.data.size());
        memcpy(option_length_bytes.data(), &net_option_length, 2);

        TRY(buffer.try_append(option.data));
    }

    return {};
}

}
