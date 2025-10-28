/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>

namespace IPC {

// Multicodec codes from https://github.com/multiformats/multicodec
enum class MulticodecCode : u64 {
    Raw = 0x55,                    // Raw binary
    DagPB = 0x70,                  // MerkleDAG protobuf
    DagCBOR = 0x71,                // MerkleDAG CBOR
    DagJSON = 0x0129,              // MerkleDAG JSON
    GitRaw = 0x78,                 // Git raw object
    EthBlock = 0x90,               // Ethereum block
    EthBlockList = 0x91,           // Ethereum block list
    BitcoinBlock = 0xb0,           // Bitcoin block
    ZcashBlock = 0xc0,             // Zcash block
    Libp2pKey = 0x72,              // Libp2p public key
};

class Multicodec {
public:
    // Get codec name from code
    static ByteString codec_name(MulticodecCode code);

    // Get codec name from u64 code
    static ByteString codec_name(u64 code);

    // Check if codec is known
    static bool is_known_codec(u64 code);

private:
    Multicodec() = delete;
};

}
