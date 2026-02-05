/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/PortBlocking.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#block-bad-port
RequestOrResponseBlocking block_bad_port(Request const& request)
{
    // 1. Let url be request’s current URL.
    auto const& url = request.current_url();

    // 2. If url’s scheme is an HTTP(S) scheme and url’s port is a bad port, then return blocked.
    if (is_http_or_https_scheme(url.scheme()) && url.port().has_value() && is_bad_port(*url.port()))
        return RequestOrResponseBlocking::Blocked;

    // 3. Return allowed.
    return RequestOrResponseBlocking::Allowed;
}

// https://fetch.spec.whatwg.org/#bad-port
bool is_bad_port(u16 port)
{
    // A port is a bad port if it is listed in the first column of the following table.
    static constexpr auto bad_ports = Array {
        1,     // tcpmux
        7,     // echo
        9,     // discard
        11,    // systat
        13,    // daytime
        15,    // netstat
        17,    // qotd
        19,    // chargen
        20,    // ftp-data
        21,    // ftp
        22,    // ssh
        23,    // telnet
        25,    // smtp
        37,    // time
        42,    // name
        43,    // nicname
        53,    // domain
        69,    // tftp
        77,    // —
        79,    // finger
        87,    // —
        95,    // supdup
        101,   // hostname
        102,   // iso-tsap
        103,   // gppitnp
        104,   // acr-nema
        109,   // pop2
        110,   // pop3
        111,   // sunrpc
        113,   // auth
        115,   // sftp
        117,   // uucp-path
        119,   // nntp
        123,   // ntp
        135,   // epmap
        137,   // netbios-ns
        139,   // netbios-ssn
        143,   // imap
        161,   // snmp
        179,   // bgp
        389,   // ldap
        427,   // svrloc
        465,   // submissions
        512,   // exec
        513,   // login
        514,   // shell
        515,   // printer
        526,   // tempo
        530,   // courier
        531,   // chat
        532,   // netnews
        540,   // uucp
        548,   // afp
        554,   // rtsp
        556,   // remotefs
        563,   // nntps
        587,   // submission
        601,   // syslog-conn
        636,   // ldaps
        989,   // ftps-data
        990,   // ftps
        993,   // imaps
        995,   // pop3s
        1719,  // h323gatestat
        1720,  // h323hostcall
        1723,  // pptp
        2049,  // nfs
        3659,  // apple-sasl
        4045,  // npp
        4190,  // sieve
        5060,  // sip
        5061,  // sips
        6000,  // x11
        6566,  // sane-port
        6665,  // ircu
        6666,  // ircu
        6667,  // ircu
        6668,  // ircu
        6669,  // ircu
        6679,  // osaut
        6697,  // ircs-u
        10080, // amanda
    };
    return binary_search(bad_ports.span(), port);
}

}
