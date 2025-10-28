/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Multicodec.h>

namespace IPC {

ByteString Multicodec::codec_name(MulticodecCode code)
{
    return codec_name(static_cast<u64>(code));
}

ByteString Multicodec::codec_name(u64 code)
{
    switch (static_cast<MulticodecCode>(code)) {
    case MulticodecCode::Raw:
        return "raw"sv;
    case MulticodecCode::DagPB:
        return "dag-pb"sv;
    case MulticodecCode::DagCBOR:
        return "dag-cbor"sv;
    case MulticodecCode::DagJSON:
        return "dag-json"sv;
    case MulticodecCode::GitRaw:
        return "git-raw"sv;
    case MulticodecCode::EthBlock:
        return "eth-block"sv;
    case MulticodecCode::EthBlockList:
        return "eth-block-list"sv;
    case MulticodecCode::BitcoinBlock:
        return "bitcoin-block"sv;
    case MulticodecCode::ZcashBlock:
        return "zcash-block"sv;
    case MulticodecCode::Libp2pKey:
        return "libp2p-key"sv;
    default:
        return ByteString::formatted("codec-{:#x}", code);
    }
}

bool Multicodec::is_known_codec(u64 code)
{
    switch (static_cast<MulticodecCode>(code)) {
    case MulticodecCode::Raw:
    case MulticodecCode::DagPB:
    case MulticodecCode::DagCBOR:
    case MulticodecCode::DagJSON:
    case MulticodecCode::GitRaw:
    case MulticodecCode::EthBlock:
    case MulticodecCode::EthBlockList:
    case MulticodecCode::BitcoinBlock:
    case MulticodecCode::ZcashBlock:
    case MulticodecCode::Libp2pKey:
        return true;
    default:
        return false;
    }
}

}
