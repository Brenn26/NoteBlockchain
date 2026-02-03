// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeminer.h"
#include "masternodeman.h"
#include "kernel.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "net.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "miner.h"
#include "init.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/sign.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utiltime.h"
#include "validation.h"
#include "validationinterface.h"
#include "wallet/wallet.h"

#include <boost/thread.hpp>

CMasternodeMiner masternodeMiner;

bool CMasternodeMiner::CanStake(const CChainParams& chainparams)
{
    const Consensus::Params& params = chainparams.GetConsensus();

    // Check if PoS is enabled
    if (!params.fAllowPoSBlocks)
        return false;

    // Check if we're past activation height
    if (chainActive.Height() < params.nMasternodeActivationHeight)
        return false;

    // In competing mode, we can always try to stake
    // Both PoW and PoS compete simultaneously
    return true;
}

bool CMasternodeMiner::SelectMasternodeCoins(std::vector<COutput>& vCoins,
                                            CAmount nTargetValue,
                                            CWallet* pwallet,
                                            const Consensus::Params& params)
{
    std::vector<COutput> vAvailableCoins;
    pwallet->AvailableCoins(vAvailableCoins, true, nullptr, MAX_MONEY, MAX_MONEY, MAX_MONEY, 0);

    // Get our external IP address (as seen by other nodes on the network)
    CService myAddr;

    // GetLocal returns address based on peer connections - prefer external
    if (!GetLocal(myAddr, nullptr)) {
        LogPrintf("CMasternodeMiner::SelectMasternodeCoins: Cannot determine external IP\n");
        return false;
    }

    // Reject private/internal IP addresses - must use public IP
    if (myAddr.IsRFC1918() || myAddr.IsLocal() || myAddr.IsInternal()) {
        LogPrintf("CMasternodeMiner::SelectMasternodeCoins: Cannot use internal IP %s - need public IP\n",
                 myAddr.ToString());
        LogPrintf("CMasternodeMiner: Please configure your external IP with -externalip=<public_ip>\n");
        return false;
    }

    // Check if our IP already has a masternode registered
    if (mnodeman.HasIP(myAddr)) {
        // Our IP is already registered, only return coins for that masternode
        CMasternode* pmn = mnodeman.FindByIP(myAddr);
        if (pmn) {
            // Find the coin matching this masternode's outpoint
            for (const COutput& out : vAvailableCoins) {
                COutPoint outpoint(out.tx->GetHash(), out.i);
                if (outpoint == pmn->outpoint) {
                    if (out.nDepth >= params.nMasternodeMinimumConfirmations) {
                        vCoins.push_back(out);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // No masternode registered for our IP yet - find eligible coins
    for (const COutput& out : vAvailableCoins) {
        // Check if this is masternode collateral (exact amount)
        if (out.tx->tx->vout[out.i].nValue == params.nMasternodeCollateral) {
            // Check maturity (minimum confirmations)
            if (out.nDepth >= params.nMasternodeMinimumConfirmations) {
                COutPoint outpoint(out.tx->GetHash(), out.i);

                // Auto-register this masternode with our IP
                CMasternode mn(outpoint, myAddr, CPubKey()); // Will set pubkey from wallet later
                if (mnodeman.Add(mn)) {
                    LogPrintf("CMasternodeMiner: Auto-registered masternode %s at IP %s\n",
                             outpoint.ToString(), myAddr.ToString());
                }

                vCoins.push_back(out);
                // Return first valid UTXO - one masternode per IP
                return true;
            }
        }
    }

    return !vCoins.empty();
}

bool CMasternodeMiner::CreateCoinStake(const CChainParams& chainparams,
                                      CWallet* pwallet,
                                      unsigned int nBits,
                                      CTransaction& txNew,
                                      unsigned int& nTxNewTime)
{
    const Consensus::Params& params = chainparams.GetConsensus();
    const CBlockIndex* pindexPrev = chainActive.Tip();

    if (!pindexPrev)
        return false;

    // Get masternode coins
    std::vector<COutput> vCoins;
    if (!SelectMasternodeCoins(vCoins, params.nMasternodeCollateral, pwallet, params))
        return false;

    // Try each coin
    for (const COutput& coin : vCoins) {
        // Get the transaction
        const CWalletTx* wtx = coin.tx;
        if (!wtx)
            continue;

        // Try different timestamps (current time + up to 30 seconds into future)
        int64_t nCurrentTime = GetAdjustedTime();

        for (unsigned int nTryTime = nCurrentTime; nTryTime <= nCurrentTime + 30; nTryTime++) {
            // Check if this coin can stake at this time
            uint256 hashProofOfStake;

            // Get the block index where this coin was created
            const CBlockIndex* pindexFrom = pindexPrev;
            while (pindexFrom && pindexFrom->nHeight > coin.nDepth) {
                pindexFrom = pindexFrom->pprev;
            }

            if (!pindexFrom)
                continue;

            const CTransaction& txPrev = *wtx->tx;
            COutPoint prevout(wtx->GetHash(), coin.i);

            if (CheckStakeKernelHash(nBits, pindexFrom, txPrev, prevout,
                                   nTryTime, hashProofOfStake, params)) {
                // Found valid stake!
                CMutableTransaction txNewMut;
                txNewMut.nTime = nTryTime;
                nTxNewTime = nTryTime;

                // Input: the masternode collateral
                txNewMut.vin.resize(1);
                txNewMut.vin[0].prevout = prevout;

                // Output 0: empty (kernel marker)
                txNewMut.vout.resize(3);
                txNewMut.vout[0].SetEmpty();

                // Output 1: return collateral to same address
                txNewMut.vout[1].nValue = params.nMasternodeCollateral;
                txNewMut.vout[1].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;

                // Output 2: stake reward
                CAmount nReward = GetBlockSubsidy(pindexPrev->nHeight + 1, params);
                txNewMut.vout[2].nValue = nReward;
                txNewMut.vout[2].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;

                // Sign the transaction
                const CKeyStore& keystore = *pwallet;
                SignatureData sigdata;

                if (!ProduceSignature(keystore, MutableTransactionSignatureCreator(&txNewMut, 0, params.nMasternodeCollateral, SIGHASH_ALL),
                                    wtx->tx->vout[coin.i].scriptPubKey, sigdata)) {
                    continue;
                }

                UpdateInput(txNewMut.vin[0], sigdata);

                txNew = CTransaction(txNewMut);
                return true;
            }
        }
    }

    return false;
}

bool CMasternodeMiner::CreateBlock(CBlock& block, CWallet* pwallet, const CChainParams& chainparams)
{
    if (!CanStake(chainparams))
        return false;

    const CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev)
        return false;

    const Consensus::Params& params = chainparams.GetConsensus();

    // Create coinbase (empty for PoS)
    CMutableTransaction txCoinbase;
    txCoinbase.vin.resize(1);
    txCoinbase.vin[0].prevout.SetNull();
    txCoinbase.vout.resize(1);
    txCoinbase.vout[0].SetEmpty();

    // Create coinstake
    CTransaction txCoinstake;
    unsigned int nTxNewTime;

    if (!CreateCoinStake(chainparams, pwallet, pindexPrev->nBits, txCoinstake, nTxNewTime))
        return false;

    // Assemble block
    block.vtx.clear();
    block.vtx.push_back(MakeTransactionRef(txCoinbase));
    block.vtx.push_back(MakeTransactionRef(txCoinstake));

    // Add transactions from mempool (optional - you can add more txs for fees)
    // For simplicity, we'll just include the coinstake for now

    // Fill header
    block.nVersion = ComputeBlockVersion(pindexPrev, params);
    block.hashPrevBlock = pindexPrev->GetBlockHash();
    block.nTime = nTxNewTime;
    block.nBits = GetNextWorkRequired(pindexPrev, &block, params);
    block.nNonce = 0; // PoS doesn't need nonce
    block.hashMerkleRoot = BlockMerkleRoot(block);

    return true;
}

int64_t CMasternodeMiner::GetExpectedStakeTime(const CChainParams& chainparams, CWallet* pwallet)
{
    const Consensus::Params& params = chainparams.GetConsensus();

    // Get number of stakeable coins
    std::vector<COutput> vCoins;
    if (!SelectMasternodeCoins(vCoins, params.nMasternodeCollateral, pwallet, params))
        return -1;

    // Simple estimate: with 30% target weight, expect stake every ~100 seconds on average
    // This is a rough estimate - actual time varies based on network difficulty
    return 100;
}

// Staking thread - runs continuously, competing with PoW miners
void ThreadStakeMinter(CWallet* pwallet)
{
    LogPrintf("ThreadStakeMinter started\n");

    try {
        while (true) {
            if (ShutdownRequested())
                break;

            // Check if we should stake
            if (!masternodeMiner.CanStake(Params())) {
                MilliSleep(10000); // Sleep 10 seconds
                continue;
            }

            // Try to create a PoS block
            CBlock block;
            if (masternodeMiner.CreateBlock(block, pwallet, Params())) {
                // Submit the block
                std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
                bool fNewBlock = false;

                if (ProcessNewBlock(Params(), shared_pblock, true, &fNewBlock)) {
                    if (fNewBlock) {
                        LogPrintf("ThreadStakeMinter: PoS block accepted! Height=%d Hash=%s\n",
                                 chainActive.Height(), block.GetHash().ToString());
                    }
                }
            }

            // Try again in 1 second (continuously competing with miners)
            MilliSleep(1000);
        }
    }
    catch (const std::exception& e) {
        LogPrintf("ThreadStakeMinter exception: %s\n", e.what());
    }
    catch (...) {
        LogPrintf("ThreadStakeMinter unknown exception\n");
    }

    LogPrintf("ThreadStakeMinter stopped\n");
}
