// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_H
#define BITCOIN_KERNEL_H

#include "primitives/transaction.h"
#include "primitives/block.h"
#include "chain.h"

class CBlockIndex;
class CCoinsViewCache;

namespace Consensus {
    struct Params;
}

// Compute the hash modifier for proof-of-stake
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier);

// Check if coinstake kernel meets protocol
// This is the core PoS validation - checks if the hash meets the target
bool CheckStakeKernelHash(unsigned int nBits,
                          const CBlockIndex* pindexFrom,
                          const CTransaction& txPrev,
                          const COutPoint& prevout,
                          unsigned int nTimeTx,
                          uint256& hashProofOfStake,
                          const Consensus::Params& params);

// Check stake kernel hash with target adjusted for competing PoW/PoS
// Uses nPoSTargetWeight to achieve 70/30 ratio
bool CheckProofOfStake(const CBlockIndex* pindexPrev,
                       const CTransaction& tx,
                       unsigned int nBits,
                       unsigned int nBlockTime,
                       uint256& hashProofOfStake,
                       CCoinsViewCache& view,
                       const Consensus::Params& params);

// Get stake age in seconds
int64_t GetStakeAge(const CTransaction& tx, unsigned int nTimeTx);

#endif // BITCOIN_KERNEL_H
