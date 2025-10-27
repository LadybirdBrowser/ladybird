/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace IPC::Limits {

// Maximum sizes for IPC messages to prevent denial-of-service attacks
// These limits are based on reasonable use cases and prevent resource exhaustion

// Overall message size limit (16 MiB)
// Rationale: Large enough for image data, small enough to prevent OOM
static constexpr size_t MaxMessageSize = 16 * 1024 * 1024;

// String length limit (1 MiB)
// Rationale: Covers long page titles, URLs, and text content
static constexpr size_t MaxStringLength = 1024 * 1024;

// Vector size limit (1M elements)
// Rationale: Allows large arrays while preventing memory exhaustion
static constexpr size_t MaxVectorSize = 1024 * 1024;

// ByteBuffer size limit (16 MiB)
// Rationale: Matches MaxMessageSize for consistency
static constexpr size_t MaxByteBufferSize = 16 * 1024 * 1024;

// HashMap size limit (100K entries)
// Rationale: Covers HTTP headers, cookies, localStorage
static constexpr size_t MaxHashMapSize = 100 * 1024;

// Nesting depth limit (recursion protection)
// Rationale: Prevents stack overflow in recursive deserialization
static constexpr size_t MaxNestingDepth = 32;

// URL length limit (per RFC 7230)
// Rationale: Most servers/browsers use 8KB limit
static constexpr size_t MaxURLLength = 8192;

// Cookie size limit (per RFC 6265)
// Rationale: Standard cookie size limit
static constexpr size_t MaxCookieSize = 4096;

// HTTP header count limit
// Rationale: Prevents header bombing attacks
static constexpr size_t MaxHTTPHeaderCount = 100;

// HTTP header value size limit
// Rationale: Reasonable size for header values
static constexpr size_t MaxHTTPHeaderValueSize = 8192;

// Image dimension limits
// Rationale: 16K x 16K is larger than any reasonable display
static constexpr u32 MaxImageWidth = 16384;
static constexpr u32 MaxImageHeight = 16384;

// File size limit for uploads (100 MiB)
// Rationale: Balance between functionality and DoS prevention
static constexpr size_t MaxFileUploadSize = 100 * 1024 * 1024;

// Maximum number of file descriptors in a single IPC message
// Rationale: Prevents file descriptor exhaustion
static constexpr size_t MaxFileDescriptorsPerMessage = 16;

// Proxy/Network security limits
// Hostname length limit (per RFC 1035)
// Rationale: DNS hostname labels are max 63 bytes, full name max 255 bytes
static constexpr size_t MaxHostnameLength = 255;

// Port number limit (well-known valid range)
// Rationale: TCP/UDP ports are 1-65535, 0 is invalid
static constexpr u16 MinPortNumber = 1;
static constexpr u16 MaxPortNumber = 65535;

// Authentication credential limits
// Rationale: Balance between compatibility and DoS prevention
static constexpr size_t MaxUsernameLength = 256;
static constexpr size_t MaxPasswordLength = 1024;

// Tor circuit ID limit
// Rationale: Tor circuit IDs are short alphanumeric strings
static constexpr size_t MaxCircuitIDLength = 128;

// Rate limiting defaults
// Rationale: Prevent IPC message flooding from compromised processes
static constexpr size_t DefaultRateLimit = 1000; // messages per second
static constexpr size_t MaxRateLimit = 10000;    // maximum configurable rate

// Proxy validation timeout (milliseconds)
// Rationale: Prevent blocking event loop during proxy validation
// Short timeout prevents UI freezes while still detecting most failures
static constexpr u32 ProxyValidationTimeoutMS = 2000; // 2 seconds

}
