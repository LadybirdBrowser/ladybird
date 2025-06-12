/*
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This header is needed because winsock2.h and windows.h are banned from including from headers and SocketAddress.h needs some struct definitions from those headers.
// These definitions were copied from Windows SDK headers. Comments, #ifdefs and special annotations like _Field_size_bytes_ were removed. This is fair use, see Oracle v. Google.

#pragma once

typedef int INT;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef const CHAR* PCSTR;
typedef USHORT ADDRESS_FAMILY;
typedef int socklen_t;

#define WINAPI_FAMILY_PARTITION(x) 1
#define FAR
#include <inaddr.h>
#undef WINAPI_FAMILY_PARTITION

#include <afunix.h>

#define AF_UNSPEC 0
#define AF_LOCAL 1 // AF_UNIX
#define AF_INET 2
#define AF_INET6 23

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

#define INADDR_LOOPBACK 0x7f000001

enum IPPROTO {
    IPPROTO_TCP = 6
};

#define INET_ADDRSTRLEN 22
#define INET6_ADDRSTRLEN 65

struct in6_addr {
    union {
        UCHAR Byte[16];
        USHORT Word[8];
    } u;
};

struct SCOPE_ID {
    union {
        struct {
            ULONG Zone : 28;
            ULONG Level : 4;
        } u;
        ULONG Value;
    } u;
};

struct sockaddr_in6 {
    ADDRESS_FAMILY sin6_family;
    USHORT sin6_port;
    ULONG sin6_flowinfo;
    in6_addr sin6_addr;
    union {
        ULONG sin6_scope_id;
        SCOPE_ID sin6_scope_struct;
    };
};

struct sockaddr_in {
    ADDRESS_FAMILY sin_family;
    USHORT sin_port;
    IN_ADDR sin_addr;
    CHAR sin_zero[8];
};

struct sockaddr {
    ADDRESS_FAMILY sa_family;
    CHAR sa_data[14];
};
using SOCKADDR = sockaddr;
using LPSOCKADDR = sockaddr*;

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    char* ai_canonname;
    sockaddr* ai_addr;
    addrinfo* ai_next;
};
using ADDRINFOA = addrinfo;
using PADDRINFOA = addrinfo*;

struct SOCKET_ADDRESS {
    sockaddr* lpSockaddr;
    INT iSockaddrLength;
};

struct SOCKET_ADDRESS_LIST {
    INT iAddressCount;
    SOCKET_ADDRESS Address[1];
};
using PSOCKET_ADDRESS_LIST = SOCKET_ADDRESS_LIST*;

struct CSADDR_INFO {
    SOCKET_ADDRESS LocalAddr;
    SOCKET_ADDRESS RemoteAddr;
    INT iSocketType;
    INT iProtocol;
};
using LPCSADDR_INFO = CSADDR_INFO*;

struct WSABUF {
    ULONG len;
    CHAR* buf;
};
using LPWSABUF = WSABUF*;

struct WSAMSG {
    LPSOCKADDR name;
    INT namelen;
    LPWSABUF lpBuffers;
    ULONG dwBufferCount;
    WSABUF Control;
    ULONG dwFlags;
};
using LPWSAMSG = WSAMSG*;

extern "C" __stdcall INT getaddrinfo(PCSTR pNodeName, PCSTR pServiceName, const ADDRINFOA* pHints, PADDRINFOA* ppResult);
extern "C" __stdcall void freeaddrinfo(PADDRINFOA pAddrInfo);
extern "C" __stdcall PCSTR inet_ntop(int Family, void const* pAddr, char* pStringBuf, size_t StringBufSize);
extern "C" __stdcall USHORT htons(USHORT hostshort);

#define _WS2DEF_ // don't include ws2def.h
