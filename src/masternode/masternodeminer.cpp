// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeminer.h"
#include "masternodeman.h"
#include "kernel.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/consensus.h"
#include "net.h"
#include "netbase.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "miner.h"
#include "init.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "script/script.h"
#include "script/sign.h"
#include "script/standard.h"
#include "keystore.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utiltime.h"
#include "validation.h"
#include "validationinterface.h"
#include "wallet/wallet.h"
#include <netmessagemaker.h>


#include <boost/thread.hpp>

CMasternodeMiner masternodeMiner;

bool CMasternodeMiner::CanStake(const CChainParams& chainparams)
{
    // Check if staking has been enabled by the user
    if (!fStakingEnabled)
        return false;

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
    // Get the external IP from config
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

    // Find our registered masternode for this IP
    CMasternode* pmn = mnodeman.FindByIP(myAddr);
    if (!pmn) {
        LogPrintf("CMasternodeMiner: No masternode found for IP %s (mnodeman has %d entries). Run 'masternode start' to activate.\n",
                 myAddr.ToString(), mnodeman.size());
        return false;
    }
    if (!pmn->IsEnabled()) {
        LogPrintf("CMasternodeMiner: Masternode at %s is not enabled (state=%s, outpoint=%s)\n",
                 myAddr.ToString(), pmn->GetStatus(), pmn->outpoint.ToString());
        return false;
    }

    // Verify we own the registered masternode's collateral
    // Note: After staking, the wallet's BlockConnected notification is async,
    // so the coinstake tx may not be in mapWallet yet. If the UTXO exists in
    // pcoinsTip as a coinstake output, wait briefly for the wallet to catch up.
    const CWalletTx* wtx = pwallet->GetWalletTx(pmn->outpoint.hash);
    if (!wtx) {
        // Check if this is a coinstake UTXO the wallet hasn't learned about yet
        Coin coin;
        if (pcoinsTip && pcoinsTip->GetCoin(pmn->outpoint, coin) && !coin.IsSpent() && coin.IsCoinStake()) {
            // Coinstake output exists on-chain but wallet is behind — poll briefly
            for (int nRetry = 0; nRetry < 20 && !wtx; nRetry++) {
                MilliSleep(250);
                wtx = pwallet->GetWalletTx(pmn->outpoint.hash);
            }
            if (wtx) {
                LogPrintf("CMasternodeMiner: Wallet caught up with coinstake %s after brief wait\n",
                         pmn->outpoint.ToString());
            } else {
                LogPrintf("CMasternodeMiner: Collateral %s not yet in wallet (async sync), but coinstake UTXO exists — skipping this round\n",
                         pmn->outpoint.ToString());
                return false;
            }
        } else if (pcoinsTip && pcoinsTip->GetCoin(pmn->outpoint, coin) && !coin.IsSpent()) {
            LogPrintf("CMasternodeMiner: Collateral %s not in wallet but UTXO exists — skipping this round\n",
                     pmn->outpoint.ToString());
            return false;
        } else {
            LogPrintf("CMasternodeMiner: Registered masternode collateral %s not found in our wallet or UTXO set\n",
                     pmn->outpoint.ToString());
            return false;
        }
    }
    if (pmn->outpoint.n >= wtx->tx->vout.size() || !pwallet->IsMine(wtx->tx->vout[pmn->outpoint.n])) {
        LogPrintf("CMasternodeMiner: Registered masternode collateral %s not owned by our wallet\n", pmn->outpoint.ToString());
        return false;
    }

    // Verify the collateral UTXO still exists and is unspent
    Coin coin;
    if (!pcoinsTip || !pcoinsTip->GetCoin(pmn->outpoint, coin) || coin.IsSpent()) {
        LogPrintf("CMasternodeMiner: Collateral UTXO %s is spent or missing\n", pmn->outpoint.ToString());
        return false;
    }

    // Verify the collateral amount
    if (coin.out.nValue != params.nMasternodeCollateral) {
        LogPrintf("CMasternodeMiner: Collateral UTXO value mismatch\n");
        return false;
    }

    // Check minimum confirmations for initial collateral
    // Exception: coinstake outputs (returned collateral from staking) can be used immediately
    // since they are already validated by the chain via CheckProofOfStake
    int nDepth = chainActive.Height() - coin.nHeight + 1;
    if (!coin.IsCoinStake() && nDepth < params.nMasternodeMinimumConfirmations) {
        LogPrintf("CMasternodeMiner: Collateral %s needs %d more confirmations (has %d)\n",
                 pmn->outpoint.ToString(),
                 params.nMasternodeMinimumConfirmations - nDepth,
                 nDepth);
        return false;
    }

    // Build a COutput directly — the collateral is locked (intentionally) so
    // AvailableCoins won't return it. We already verified ownership, amount,
    // unspent status, and confirmations above, so construct COutput manually.
    LogPrintf("CMasternodeMiner: Using locked collateral %s at %s\n",
             pmn->outpoint.ToString(), pmn->addr.ToString());

    vCoins.push_back(COutput(wtx, pmn->outpoint.n, nDepth, true, true, true));
    return true;
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
        return false;
    }

    // Get masternode coins
    std::vector<COutput> vCoins;
    if (!SelectMasternodeCoins(vCoins, params.nMasternodeCollateral, pwallet, params))
        return false;

    // Try each coin
    for (const COutput& coin : vCoins) {
        const CWalletTx* wtx = coin.tx;
        if (!wtx)
            continue;

        COutPoint prevout(wtx->GetHash(), coin.i);

        // Try different timestamps. Must be at least pindexPrev->nTime + 15
        // to satisfy the PoS minimum gap rule enforced in ConnectBlock.
        int64_t nCurrentTime = GetAdjustedTime();
        int64_t nMinTime = (int64_t)pindexPrev->nTime + 16; // +16 to be safely above the >15 check
        if (nCurrentTime < nMinTime)
            nCurrentTime = nMinTime;

        LogPrintf("CMasternodeMiner: Attempting to stake coin %s:%d (value=%s, depth=%d)\n",
                 wtx->GetHash().ToString().substr(0,10), coin.i,
                 FormatMoney(coin.tx->tx->vout[coin.i].nValue), coin.nDepth);

        for (unsigned int nTryTime = nCurrentTime; nTryTime <= nCurrentTime + 30; nTryTime++) {
            uint256 hashProofOfStake;

            // Get the block index where this coin was created
            const CBlockIndex* pindexFrom = pindexPrev;
            for (int i = 0; i < coin.nDepth - 1 && pindexFrom; i++) {
                pindexFrom = pindexFrom->pprev;
            }

            if (!pindexFrom) {
                LogPrintf("CMasternodeMiner: Failed to find pindexFrom for coin\n");
                continue;
            }

            const CTransaction& txPrev = *wtx->tx;

            if (CheckStakeKernelHash(nBits, pindexFrom, txPrev, prevout,
                                   nTryTime, hashProofOfStake, params)) {
                // Found valid stake! Build the coinstake transaction.
                // The collateral is spent and returned in a new output (normal UTXO flow).
                CMutableTransaction txNewMut;
                nTxNewTime = nTryTime;

                // Input: spend the collateral outpoint
                txNewMut.vin.resize(1);
                txNewMut.vin[0].prevout = prevout;

                // Calculate block reward
                CAmount nReward = GetBlockSubsidy(pindexPrev->nHeight + 1, params);
                CAmount nCollateral = wtx->tx->vout[coin.i].nValue;

                LogPrintf("CMasternodeMiner::CreateCoinStake: height=%d collateral=%s reward=%s total_out=%s\n",
                         pindexPrev->nHeight + 1, FormatMoney(nCollateral), FormatMoney(nReward),
                         FormatMoney(nCollateral + nReward));

                // Output 0: empty marker (keeps IsCoinStake() = true)
                // Output 1: collateral returned to same address
                // Output 2: block reward to the masternode operator
                txNewMut.vout.resize(3);
                txNewMut.vout[0].SetEmpty();
                txNewMut.vout[1].nValue = nCollateral;
                txNewMut.vout[1].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;
                txNewMut.vout[2].nValue = nReward;
                txNewMut.vout[2].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;

                // Sign the input to prove we own the collateral's private key
                const CKeyStore& keystore = *pwallet;
                SignatureData sigdata;

                if (!ProduceSignature(MutableTransactionSignatureCreator(&keystore, &txNewMut, 0, params.nMasternodeCollateral, SIGHASH_ALL),
                                    wtx->tx->vout[coin.i].scriptPubKey, sigdata)) {
                    continue;
                }

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

    // Calculate the next PoS difficulty target
    // This uses a separate difficulty calculation that only considers PoS blocks
    // This allows PoS and PoW to have independent difficulty adjustments
    unsigned int nBits = GetNextPoSRequired(pindexPrev, params);

    LogPrintf("CMasternodeMiner::CreateBlock: Using PoS difficulty nBits=%08x (separate from PoW)\n", nBits);

    // Create coinbase (empty for PoS, but must include block height per BIP34)
    CMutableTransaction txCoinbase;
    txCoinbase.vin.resize(1);
    txCoinbase.vin[0].prevout.SetNull();
    // BIP34: Coinbase must include block height in scriptSig
    txCoinbase.vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1);
    txCoinbase.vout.resize(1);
    txCoinbase.vout[0].SetEmpty();

    // Create coinstake using the ADJUSTED difficulty (not previous block's difficulty)
    // This allows PoS to benefit from difficulty decreases when blocks are slow
    CMutableTransaction txCoinstake;
    unsigned int nTxNewTime;

    if (!CreateCoinStake(chainparams, pwallet, nBits, txCoinstake, nTxNewTime))
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
    block.nBits = nBits;  // Use the pre-calculated adjusted difficulty
    block.nNonce = 0; // PoS doesn't need nonce
    block.hashMerkleRoot = BlockMerkleRoot(block);

    return true;
}

int64_t CMasternodeMiner::GetExpectedStakeTime(const CChainParams& chainparams, CWallet* pwallet)
{
    const Consensus::Params& params = chainparams.GetConsensus();

    // Check if we have stakeable coins
    std::vector<COutput> vCoins;
    if (!SelectMasternodeCoins(vCoins, params.nMasternodeCollateral, pwallet, params))
        return -1;

    // Estimate: PoS targets 45s spacing. With N masternodes competing,
    // each one expects a block every 45 * N seconds on average.
    int nEnabledMasternodes = mnodeman.CountEnabled();
    if (nEnabledMasternodes <= 0)
        nEnabledMasternodes = 1;

    const int64_t nPoSTargetSpacing = 45; // seconds between PoS blocks
    return nPoSTargetSpacing * nEnabledMasternodes;
}

// Staking thread - runs continuously, competing with PoW miners
void ThreadStakeMinter(CWallet* pwallet)
{
    LogPrintf("ThreadStakeMinter started\n");

    int nCleanupCounter = 0;
    int nLogCounter = 0;
    bool fPreviouslyEnabled = false;
    try {
        while (true) {
            if (ShutdownRequested())
                break;

            // Periodically clean up expired masternodes (every 60 iterations = ~1 minute)
            if (++nCleanupCounter >= 60) {
                mnodeman.CheckAndRemove();
                nCleanupCounter = 0;
            }

            // Check if we should stake
            bool fCanStake = masternodeMiner.CanStake(Params());

            // Log status changes
            if (fCanStake && !fPreviouslyEnabled) {
                LogPrintf("ThreadStakeMinter: Staking ENABLED - attempting to find PoS blocks\n");
                fPreviouslyEnabled = true;
            } else if (!fCanStake && fPreviouslyEnabled) {
                LogPrintf("ThreadStakeMinter: Staking DISABLED\n");
                fPreviouslyEnabled = false;
            }

            // Periodically log status when disabled (every 60 seconds)
            if (!fCanStake) {
                if (++nLogCounter >= 6) {
                    if (!masternodeMiner.IsStakingEnabled()) {
                        LogPrintf("ThreadStakeMinter: Waiting for staking to be enabled. Run 'masternode start' to begin staking.\n");
                    } else if (chainActive.Height() < Params().GetConsensus().nMasternodeActivationHeight) {
                        LogPrintf("ThreadStakeMinter: Waiting for PoS activation at height %d (current: %d)\n",
                                 Params().GetConsensus().nMasternodeActivationHeight, chainActive.Height());
                    } else {
                        LogPrintf("ThreadStakeMinter: Staking disabled (check requirements)\n");
                    }
                    nLogCounter = 0;
                }
                MilliSleep(10000); // Sleep 10 seconds
                continue;
            }

            nLogCounter = 0;

            // Try to create a PoS block
            CBlock block;
            if (masternodeMiner.CreateBlock(block, pwallet, Params())) {
                // Submit the block
                std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
                bool fNewBlock = false;

                ProcessNewBlock(Params(), shared_pblock, true, &fNewBlock);

                // CRITICAL: Verify the block was actually accepted into the active chain.
                // ProcessNewBlock returns true even when ConnectBlock fails (e.g. timestamp
                // too close). We must check chainActive.Tip() matches our block before
                // updating the masternode outpoint, otherwise we'd point at a non-existent UTXO.
                {
                    LOCK(cs_main);
                    if (chainActive.Tip()->GetBlockHash() == block.GetHash()) {
                        // Block is confirmed in active chain — safe to proceed
                    } else {
                        LogPrintf("ThreadStakeMinter: Block %s was NOT accepted into active chain (tip is %s at height %d)\n",
                                 block.GetHash().ToString().substr(0, 10),
                                 chainActive.Tip()->GetBlockHash().ToString().substr(0, 10),
                                 chainActive.Height());
                        MilliSleep(1000);
                        continue; // Skip outpoint update, try again next iteration
                    }
                }

                // Wait for the wallet's async BlockConnected to process the
                // coinstake tx. Poll every 100ms for up to 5 seconds.
                const uint256& csTxid = block.vtx[1]->GetHash();
                for (int nWait = 0; nWait < 50; nWait++) {
                    MilliSleep(100);
                    if (pwallet->GetWalletTx(csTxid))
                        break;
                }
                if (!pwallet->GetWalletTx(csTxid)) {
                    LogPrintf("ThreadStakeMinter: WARNING - wallet did not sync coinstake tx %s within 5 seconds\n",
                             csTxid.ToString().substr(0, 10));
                }

                const CTransaction& cs = *block.vtx[1];
                LogPrintf("ThreadStakeMinter: Masternode block accepted! Height=%d Hash=%s "
                         "coinstake_txid=%s vout_count=%d vout[1]=%s vout[2]=%s\n",
                         chainActive.Height(), block.GetHash().ToString(),
                         cs.GetHash().ToString().substr(0,10),
                         (int)cs.vout.size(),
                         FormatMoney(cs.vout.size() > 1 ? cs.vout[1].nValue : 0),
                         FormatMoney(cs.vout.size() > 2 ? cs.vout[2].nValue : 0));

                // The coinstake spent the old collateral and created a new one in vout[1].
                // We need to: unlock old outpoint, lock new outpoint, update MN registration.
                const CTransaction& coinstake = *block.vtx[1];
                COutPoint oldOutpoint = coinstake.vin[0].prevout;
                COutPoint newOutpoint(coinstake.GetHash(), 1); // vout[1] = returned collateral

                {
                    LOCK(pwallet->cs_wallet);

                    // Unlock the old (now spent) collateral
                    pwallet->UnlockCoin(oldOutpoint);

                    // Lock the new collateral outpoint
                    pwallet->LockCoin(newOutpoint);
                }

                // Update masternode registration to point to new collateral.
                // Note: ConnectBlock already re-keyed the outpoint, but we also
                // re-sign here so the updated MNANNOUNCE can be broadcast to peers.
                std::string externalIP = gArgs.GetArg("-externalip", "");
                CService myAddr;
                CMasternode* pmn = nullptr;

                if (!externalIP.empty() && Lookup(externalIP.c_str(), myAddr, Params().GetDefaultPort(), false)) {
                    pmn = mnodeman.FindByIP(myAddr);
                }

                if (pmn) {
                    COutPoint pmnOldOutpoint = pmn->outpoint;
                    CMasternode mnCopy = *pmn;
                    mnCopy.outpoint = newOutpoint;
                    mnCopy.UpdateLastSeen();

                    // Re-sign with the new outpoint so peers can verify
                    CKey key;
                    {
                        LOCK(pwallet->cs_wallet);
                        pwallet->GetKey(mnCopy.pubKeyMasternode.GetID(), key);
                    }
                    if (key.IsValid()) {
                        mnCopy.Sign(key);
                        LogPrintf("ThreadStakeMinter: Re-signed masternode with new outpoint %s\n",
                                 newOutpoint.ToString());
                    } else {
                        LogPrintf("ThreadStakeMinter: WARNING - failed to get key for re-signing masternode\n");
                    }

                    // Re-index in masternode manager
                    mnodeman.Remove(pmnOldOutpoint);
                    mnodeman.Add(mnCopy);
                    mnodeman.Save();

                    // Reset cleanup counter
                    nCleanupCounter = 0;

                    // Broadcast updated masternode to network
                    if (g_connman) {
                        g_connman->ForEachNode([&mnCopy](CNode* pnode) {
                            g_connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::MNANNOUNCE, mnCopy));
                        });
                        LogPrintf("ThreadStakeMinter: Broadcast updated masternode %s to network\n",
                                 newOutpoint.ToString());
                    }

                    LogPrintf("ThreadStakeMinter: Updated masternode collateral %s -> %s (saved to disk)\n",
                             oldOutpoint.ToString(), newOutpoint.ToString());
                } else {
                    // MN entry not found — re-create from scratch
                    LogPrintf("ThreadStakeMinter: Masternode entry lost, re-creating from new outpoint %s\n",
                             newOutpoint.ToString());

                    if (!externalIP.empty() && Lookup(externalIP.c_str(), myAddr, Params().GetDefaultPort(), false)) {
                        const CTxOut& collateralOut = coinstake.vout[1];
                        CTxDestination dest;
                        if (ExtractDestination(collateralOut.scriptPubKey, dest)) {
                            LOCK(pwallet->cs_wallet);
                            CKeyID keyID = GetKeyForDestination(*pwallet, dest);
                            CPubKey pubKey;
                            CKey key;
                            if (pwallet->GetPubKey(keyID, pubKey) && pwallet->GetKey(keyID, key)) {
                                CMasternode mn(newOutpoint, myAddr, pubKey);
                                mn.Sign(key);
                                mnodeman.Add(mn);
                                mnodeman.Save();
                                nCleanupCounter = 0;

                                if (g_connman) {
                                    g_connman->ForEachNode([&mn](CNode* pnode) {
                                        g_connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::MNANNOUNCE, mn));
                                    });
                                }
                                LogPrintf("ThreadStakeMinter: Re-created masternode %s at %s\n",
                                         newOutpoint.ToString(), myAddr.ToString());
                            } else {
                                LogPrintf("ThreadStakeMinter: ERROR - failed to get keys for re-creating masternode\n");
                            }
                        }
                    }
                }
            } // if (masternodeMiner.CreateBlock)

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
