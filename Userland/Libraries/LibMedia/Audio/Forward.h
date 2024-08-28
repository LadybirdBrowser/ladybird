/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Audio {

class ConnectionToServer;
class Loader;
class Encoder;
struct Person;
struct Metadata;
class PlaybackStream;
struct Sample;

template<typename SampleType>
class ResampleHelper;

}
