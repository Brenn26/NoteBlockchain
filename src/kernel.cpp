// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "kernel.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/params.h"
#include "hash.h"
#include "streams.h"
#include "uint256.h"
#include "arith_uint256.h"
#include "util.h"
#include "timedata.h"      // For GetAdjustedTime()
#include "primitives/transaction.h"  // For CMutableTransaction



// Compute stake modifier for the next block
// Mixes previous modifier with previous block hash for randomness
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier)
{
    if (!pindexPrev) {
        nStakeModifier = 0;
        return true;
    }

    // Mix previous stake modifier with previous block hash
    // This provides randomness and prevents pre-computation attacks
    CHashWriter ss(SER_GETHASH, 0);
    ss << pindexPrev->nStakeModifier << pindexPrev->GetBlockHash();
    nStakeModifier = ss.GetHash().GetUint64(0);

    return true;
}

// Check if the coinstake kernel hash meets the difficulty target
bool CheckStakeKernelHash(unsigned int nBits,
                          const CBlockIndex* pindexFrom,
                          const CTransaction& txPrev,
                          const COutPoint& prevout,
                          unsigned int nTimeTx,
                          uint256& hashProofOfStake,
                          const Consensus::Params& params)
{
    // Check that the output exists and is the right value
    if (prevout.n >= txPrev.vout.size())
        return false;

    const CTxOut& prevTxOut = txPrev.vout[prevout.n];

    // Must be masternode collateral amount
    if (prevTxOut.nValue != params.nMasternodeCollateral)
        return false;

    // Get transaction time
    unsigned int nTimeBlockFrom = pindexFrom->nTime;

    // Check minimum stake age
    int64_t nStakeAge = nTimeTx - nTimeBlockFrom;
    if (nStakeAge < params.nStakeMinAge) {
        LogPrintf("CheckStakeKernelHash: stake age %d < minimum %d\n", nStakeAge, params.nStakeMinAge);
        return false;
    }

    // Check maximum stake age if set
    if (params.nStakeMaxAge > 0 && nStakeAge > params.nStakeMaxAge) {
        LogPrintf("CheckStakeKernelHash: stake age %d > maximum %d\n", nStakeAge, params.nStakeMaxAge);
        return false;
    }

    // Get stake modifier from previous block
    uint64_t nStakeModifier = pindexFrom->nStakeModifier;

    // Calculate the hash: hash(stakeModifier + txPrevTime + txPrevHash + txPrevN + txTime)
    CHashWriter ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << nTimeBlockFrom << prevout.hash << prevout.n << nTimeTx;
    hashProofOfStake = ss.GetHash();

    // Get the difficulty target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Apply PoS weight factor to make PoS easier than PoW
    // With nPoSTargetWeight = 300, we divide by (300/1000) = 0.3
    // This makes PoS target 3.33x larger (easier) than PoW
    // Example: 1000/300 = 3.33, making PoS easier to find blocks
    bnTarget = (bnTarget * 1000) / params.nPoSTargetWeight;

    // Calculate weight based on coin value and age
    // More coins and longer age = higher weight = easier to stake
    arith_uint256 bnCoinDayWeight = arith_uint256(prevTxOut.nValue / COIN) * nStakeAge / (24 * 60 * 60);

    // Target is multiplied by coin-day weight
    arith_uint256 bnTargetProofOfStake = bnTarget * bnCoinDayWeight;

    // Check if hash meets target
    bool fSuccess = UintToArith256(hashProofOfStake) <= bnTargetProofOfStake;

    LogPrintf("CheckStakeKernelHash: age=%d weight=%s target=%s hash=%s result=%s\n",
            nStakeAge, bnCoinDayWeight.ToString(), bnTargetProofOfStake.ToString(),
            UintToArith256(hashProofOfStake).ToString(), fSuccess ? "SUCCESS" : "fail");

    return fSuccess;
}

// Verify proof-of-stake for a coinstake transaction
bool CheckProofOfStake(const CBlockIndex* pindexPrev,
                       const CTransaction& tx,
                       unsigned int nBits,
                       unsigned int nBlockTime,
                       uint256& hashProofOfStake,
                       CCoinsViewCache& view,
                       const Consensus::Params& params)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake: called on non-coinstake transaction");

    // First input must be from masternode collateral
    const CTxIn& txin = tx.vin[0];

    // Get the previous transaction output
    Coin coin;
    if (!view.GetCoin(txin.prevout, coin)) {
        LogPrintf("CheckProofOfStake: Looking for UTXO %s but not found in view\n", txin.prevout.ToString());
        return error("CheckProofOfStake: input not found in UTXO set");
    }
    LogPrintf("CheckProofOfStake: Found UTXO %s in view\n", txin.prevout.ToString());

    // Get the block containing the input transaction
    const CBlockIndex* pindexFrom = nullptr;
    if (!coin.nHeight || coin.nHeight > pindexPrev->nHeight)
        return error("CheckProofOfStake: input block not found or too new");

    pindexFrom = pindexPrev;
    while (pindexFrom && pindexFrom->nHeight > (int)coin.nHeight)
        pindexFrom = pindexFrom->pprev;

    if (!pindexFrom)
        return error("CheckProofOfStake: block index not found for input");

    // Use CMutableTransaction to build a modifiable transaction
    CMutableTransaction mutableTxPrev;
    mutableTxPrev.vout.resize(txin.prevout.n + 1);
    mutableTxPrev.vout[txin.prevout.n] = coin.out;

    // Convert to immutable CTransaction for the check
    const CTransaction txPrev(mutableTxPrev);

    // Verify the stake kernel hash using block time
    return CheckStakeKernelHash(nBits, pindexFrom, txPrev, txin.prevout,
                               nBlockTime, hashProofOfStake, params);
}

// Get the age of the stake in seconds
// Note: Since transactions no longer have nTime, this function is deprecated
// Stake age is now calculated based on UTXO creation block time
int64_t GetStakeAge(const CTransaction& tx, unsigned int nTimeTx)
{
    // This function is kept for API compatibility but no longer uses tx.nTime
    // Stake age should be calculated from UTXO block time instead
    return 0;
}