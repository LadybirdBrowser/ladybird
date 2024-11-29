/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef USHORT ADDRESS_FAMILY;

#define WINAPI_FAMILY_PARTITION(x) 1
#define FAR
#include <inaddr.h>
#undef WINAPI_FAMILY_PARTITION
#undef FAR

#include <afunix.h>

#define AF_UNSPEC 0
#define AF_LOCAL 1 // AF_UNIX
#define AF_INET 2
#define AF_INET6 23

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

extern "C" USHORT __stdcall htons(USHORT hostshort);
