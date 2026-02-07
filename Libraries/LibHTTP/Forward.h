/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace HTTP {

class CacheEntry;
class CacheEntryReader;
class CacheEntryWriter;
class CacheIndex;
class CacheRequest;
class DiskCache;
class HeaderList;
class HttpRequest;
class HttpResponse;
class MemoryCache;

struct Header;

}

namespace HTTP::Cookie {

struct Cookie;
struct ParsedCookie;
struct VersionedCookie;

enum class IncludeCredentials : u8;
enum class SameSite : u8;
enum class Source : u8;

}
