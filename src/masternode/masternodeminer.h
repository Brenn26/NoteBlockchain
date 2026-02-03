// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_MASTERNODEMINER_H
#define BITCOIN_MASTERNODE_MASTERNODEMINER_H

#include "primitives/transaction.h"
#include "primitives/block.h"
#include "wallet/wallet.h"

class CBlockIndex;
class CCoinsViewCache;
class CChainParams;

namespace Consensus {
    struct Params;
}

/**
 * Masternode Miner - Creates PoS blocks that compete with PoW blocks
 */
class CMasternodeMiner
{
public:
    CMasternodeMiner() {}

    // Create a PoS block (competing with PoW miners)
    bool CreateBlock(CBlock& block, CWallet* pwallet, const CChainParams& chainparams);

    // Create coinstake transaction
    bool CreateCoinStake(const CChainParams& chainparams,
                        CWallet* pwallet,
                        unsigned int nBits,
                        CMutableTransaction& txNew,
                        unsigned int& nTxNewTime);

    // Find stakeable masternode coins in wallet
    bool SelectMasternodeCoins(std::vector<COutput>& vCoins,
                              CAmount nTargetValue,
                              CWallet* pwallet,
                              const Consensus::Params& params);

    // Check if we can stake now
    bool CanStake(const CChainParams& chainparams);

    // Get expected time until next stake (estimate)
    int64_t GetExpectedStakeTime(const CChainParams& chainparams, CWallet* pwallet);
};

// Global masternode miner
extern CMasternodeMiner masternodeMiner;

// Thread function for staking
void ThreadStakeMinter(CWallet* pwallet);

#endif // BITCOIN_MASTERNODE_MASTERNODEMINER_H
