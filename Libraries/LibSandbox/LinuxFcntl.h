/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <fcntl.h>

// Linux assigns F_DUPFD_QUERY the command F_LINUX_SPECIFIC_BASE + 3.
#ifndef F_DUPFD_QUERY
#    define F_DUPFD_QUERY 1027
#endif
