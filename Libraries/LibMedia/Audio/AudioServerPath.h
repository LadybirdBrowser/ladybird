/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>

namespace Audio {

// The UNIX sockets the audio library may connect to, for a caller that has to know them in advance,
// such as a sandbox allowlist.
//
// This asks the audio library where it connects rather than working it out. Which socket it picks
// depends on an environment variable, on a configuration file named by another environment
// variable, on the fragments and includes beside that file, and on a property of the X11 root
// window. Predicting all of that means reimplementing someone else's lookup and staying in step
// with it forever, and being wrong means an endpoint the sandbox refuses.
//
// The default runtime socket is listed as well, so a server that is not running when we ask can
// still be reached once it starts.
//
// A server reached over TCP is not listed. A process that cannot open a socket cannot be given one
// without also being given the network.
MEDIA_API Vector<ByteString> audio_server_path_candidates();

// The socket a PulseAudio server string names, or nothing when it does not name one. The string
// keeps whatever form it was configured in, so "unix:/run/user/1000/pulse/native" and the bare path
// are both this same socket, while a host name is a server we cannot hand to a process that has no
// sockets of its own.
MEDIA_API Optional<ByteString> unix_path_from_server_string(StringView);

}
