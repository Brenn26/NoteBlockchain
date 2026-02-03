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
#include "netbase.h"
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

    LogPrintf("CMasternodeMiner: AvailableCoins returned %d coins, looking for %s\n",
             vAvailableCoins.size(), FormatMoney(params.nMasternodeCollateral));

    // Get the external IP from config (same as GUI/RPC uses)
    std::string externalIP = gArgs.GetArg("-externalip", "");
    if (externalIP.empty()) {
        LogPrintf("CMasternodeMiner: No external IP configured. Add 'externalip=YOUR.IP.ADDRESS' to noteblockchain.conf\n");
        return false;
    }

    CService myAddr;
    if (!Lookup(externalIP.c_str(), myAddr, Params().GetDefaultPort(), false)) {
        LogPrintf("CMasternodeMiner: Failed to lookup external IP address: %s\n", externalIP);
        return false;
    }

    LogPrintf("CMasternodeMiner: Our configured IP: %s\n", myAddr.ToString());

    // Find masternodes that match BOTH our wallet UTXOs AND our configured external IP
    for (const COutput& out : vAvailableCoins) {
        LogPrintf("CMasternodeMiner:   Checking coin %s:%d value=%s depth=%d\n",
                 out.tx->GetHash().ToString().substr(0,10), out.i,
                 FormatMoney(out.tx->tx->vout[out.i].nValue), out.nDepth);
        if (out.tx->tx->vout[out.i].nValue == params.nMasternodeCollateral) {
            COutPoint outpoint(out.tx->GetHash(), out.i);

            // Check if this UTXO is registered as a masternode
            CMasternode* pmn = mnodeman.Find(outpoint);
            if (pmn && pmn->IsEnabled()) {
                // Check if this masternode is registered with OUR IP
                if (pmn->addr == myAddr) {
                    // Found our masternode (matching both UTXO and IP)
                    if (out.nDepth >= params.nMasternodeMinimumConfirmations) {
                        LogPrintf("CMasternodeMiner: Using registered masternode %s at %s\n",
                                 outpoint.ToString(), pmn->addr.ToString());
                        vCoins.push_back(out);
                        return true;
                    } else {
                        LogPrintf("CMasternodeMiner: Masternode %s needs %d more confirmations (has %d)\n",
                                 outpoint.ToString(),
                                 params.nMasternodeMinimumConfirmations - out.nDepth,
                                 out.nDepth);
                    }
                } else {
                    LogPrintf("CMasternodeMiner: Found masternode %s but it's registered to %s (we are %s)\n",
                             outpoint.ToString(), pmn->addr.ToString(), myAddr.ToString());
                }
            }
        }
    }

    LogPrintf("CMasternodeMiner: No active masternodes found matching IP %s\n", myAddr.ToString());
    LogPrintf("CMasternodeMiner: Use 'masternode start' command to register a masternode\n");
    return false;
}

bool CMasternodeMiner::CreateCoinStake(const CChainParams& chainparams,
                                      CWallet* pwallet,
                                      unsigned int nBits,
                                      CMutableTransaction& txNew,
                                      unsigned int& nTxNewTime)
{
    const Consensus::Params& params = chainparams.GetConsensus();
    const CBlockIndex* pindexPrev = chainActive.Tip();

    if (!pindexPrev)
        return false;

    // Check if wallet is unlocked for staking
    if (pwallet->IsLocked()) {
        // Wallet is locked, cannot stake
        return false;
    }

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
            // coin.nDepth is confirmations, so we go back that many blocks from tip
            const CBlockIndex* pindexFrom = pindexPrev;
            for (int i = 0; i < coin.nDepth - 1 && pindexFrom; i++) {
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

                if (!ProduceSignature(MutableTransactionSignatureCreator(&keystore, &txNewMut, 0, params.nMasternodeCollateral, SIGHASH_ALL),
                                    wtx->tx->vout[coin.i].scriptPubKey, sigdata)) {
                    continue;
                }

                // Update transaction input with signature
                txNewMut.vin[0].scriptSig = sigdata.scriptSig;
                txNewMut.vin[0].scriptWitness = sigdata.scriptWitness;

                txNew = txNewMut;
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
    CMutableTransaction txCoinstake;
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
