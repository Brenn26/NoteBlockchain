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
    std::vector<COutput> vAvailableCoins;
    // Get all available coins (use default parameters)
    pwallet->AvailableCoins(vAvailableCoins);

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

    // Check if WE (this wallet) have a masternode registered for our IP
    // We need to verify the masternode belongs to US, not just any wallet on this IP
    // This prevents auto-registration if another wallet on the same IP has a masternode
    bool fHadMasternodeForIP = false;
    std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
    for (const auto& mn : vMasternodes) {
        if (mn.addr == myAddr) {
            // Found a masternode with our IP - check if we own its collateral
            const CWalletTx* wtx = pwallet->GetWalletTx(mn.outpoint.hash);
            if (wtx && mn.outpoint.n < wtx->tx->vout.size()) {
                if (pwallet->IsMine(wtx->tx->vout[mn.outpoint.n])) {
                    // This masternode belongs to us!
                    fHadMasternodeForIP = true;
                    LogPrintf("CMasternodeMiner: Found our own masternode for IP %s (may need re-registration)\n",
                             myAddr.ToString());
                    break;
                }
            }
        }
    }

    // Find masternodes that match BOTH our wallet UTXOs AND our configured external IP
    for (const COutput& out : vAvailableCoins) {
        LogPrintf("CMasternodeMiner:   Checking coin %s:%d value=%s depth=%d\n",
                 out.tx->GetHash().ToString().substr(0,10), out.i,
                 FormatMoney(out.tx->tx->vout[out.i].nValue), out.nDepth);
        if (out.tx->tx->vout[out.i].nValue == params.nMasternodeCollateral) {
            COutPoint outpoint(out.tx->GetHash(), out.i);

            // Check if mature enough to stake
            if (out.nDepth < params.nMasternodeMinimumConfirmations) {
                LogPrintf("CMasternodeMiner: Collateral %s needs %d more confirmations (has %d)\n",
                         outpoint.ToString(),
                         params.nMasternodeMinimumConfirmations - out.nDepth,
                         out.nDepth);
                continue;
            }

            // Check if this UTXO is registered as a masternode
            CMasternode* pmn = mnodeman.Find(outpoint);
            if (pmn && pmn->IsEnabled() && pmn->addr == myAddr) {
                // Already registered with correct IP - use it for staking
                LogPrintf("CMasternodeMiner: Using registered masternode %s at %s\n",
                         outpoint.ToString(), pmn->addr.ToString());
                vCoins.push_back(out);
                return true;
            }

            // If we previously had a masternode for this IP, allow re-registration
            // This handles the case where we staked and the UTXO changed
            if (fHadMasternodeForIP) {
                LogPrintf("CMasternodeMiner: Re-registering masternode with new collateral %s (previous collateral was spent)\n",
                         outpoint.ToString());

                // Clean up any masternodes with spent UTXOs
                mnodeman.CheckAndRemove();

                // Get the public key and private key for this output
                CTxDestination dest;
                if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, dest)) {
                    LogPrintf("CMasternodeMiner: Failed to extract destination from output\n");
                    continue;
                }

                CKeyID keyID = GetKeyForDestination(*pwallet, dest);
                CPubKey pubKey;
                if (!pwallet->GetPubKey(keyID, pubKey)) {
                    LogPrintf("CMasternodeMiner: Failed to get public key\n");
                    continue;
                }

                // Get the private key for signing
                CKey key;
                if (!pwallet->GetKey(keyID, key)) {
                    LogPrintf("CMasternodeMiner: Failed to get private key for signing\n");
                    continue;
                }

                // Remove any existing masternode with our IP (old spent UTXO)
                if (mnodeman.HasIP(myAddr)) {
                    CMasternode* existingMN = mnodeman.FindByIP(myAddr);
                    if (existingMN) {
                        mnodeman.Remove(existingMN->outpoint);
                        LogPrintf("CMasternodeMiner: Removed old masternode entry %s\n",
                                 existingMN->outpoint.ToString());
                    }
                }

                // Verify UTXO still exists
                Coin coin;
                if (!pcoinsTip || !pcoinsTip->GetCoin(outpoint, coin) || coin.IsSpent()) {
                    LogPrintf("CMasternodeMiner: UTXO %s was spent during registration attempt, skipping\n",
                             outpoint.ToString());
                    continue;
                }

                // Create and sign masternode entry
                CMasternode mn(outpoint, myAddr, pubKey);
                if (!mn.Sign(key)) {
                    LogPrintf("CMasternodeMiner: Failed to sign masternode announcement\n");
                    continue;
                }

                if (mnodeman.Add(mn)) {
                    LogPrintf("CMasternodeMiner: Successfully re-registered masternode with collateral %s at %s\n",
                             outpoint.ToString(), myAddr.ToString());

                    // Broadcast to network
                    if (g_connman) {
                        g_connman->ForEachNode([&mn](CNode* pnode) {
                            g_connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::MNANNOUNCE, mn));
                        });
                        LogPrintf("CMasternodeMiner: Broadcast re-registered masternode to network\n");
                    }

                    vCoins.push_back(out);
                    return true;
                } else {
                    LogPrintf("CMasternodeMiner: Failed to add masternode\n");
                }
            } else {
                // NOT auto-registering - user must explicitly run 'masternode start'
                // This prevents masternodes from becoming active without user consent
                LogPrintf("CMasternodeMiner: Found collateral %s but not registered. Run 'masternode start' to activate.\n",
                         outpoint.ToString());
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

        COutPoint prevout(wtx->GetHash(), coin.i);

        // Lock the coin to prevent it from being spent by regular transactions
        // while we're attempting to stake with it
        pwallet->LockCoin(prevout);

        // Try different timestamps (current time + up to 30 seconds into future)
        int64_t nCurrentTime = GetAdjustedTime();

        LogPrintf("CMasternodeMiner: Attempting to stake coin %s:%d (value=%s, depth=%d)\n",
                 wtx->GetHash().ToString().substr(0,10), coin.i,
                 FormatMoney(coin.tx->tx->vout[coin.i].nValue), coin.nDepth);

        bool fStakeSuccess = false;
        for (unsigned int nTryTime = nCurrentTime; nTryTime <= nCurrentTime + 30; nTryTime++) {
            // Check if this coin can stake at this time
            uint256 hashProofOfStake;

            // Get the block index where this coin was created
            // coin.nDepth is confirmations, so we go back that many blocks from tip
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
                // Found valid stake!
                CMutableTransaction txNewMut;
                nTxNewTime = nTryTime;

                // Input: the masternode collateral
                txNewMut.vin.resize(1);
                txNewMut.vin[0].prevout = prevout;

                // Calculate stake reward
                CAmount nReward = GetBlockSubsidy(pindexPrev->nHeight + 1, params);

                // Output 0: empty (kernel marker)
                txNewMut.vout.resize(3);
                txNewMut.vout[0].SetEmpty();

                // Output 1: return collateral to same address
                txNewMut.vout[1].nValue = params.nMasternodeCollateral;
                txNewMut.vout[1].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;

                // Output 2: stake reward to same address
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
                fStakeSuccess = true;
                // Keep the coin locked - it will be spent by the coinstake transaction
                return true;
            }
        }

        // If we didn't successfully stake, unlock the coin so it can be used for other transactions
        if (!fStakeSuccess) {
            pwallet->UnlockCoin(prevout);
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

                if (ProcessNewBlock(Params(), shared_pblock, true, &fNewBlock)) {
                    if (fNewBlock) {
                        LogPrintf("ThreadStakeMinter: PoS block accepted! Height=%d Hash=%s\n",
                                 chainActive.Height(), block.GetHash().ToString());
                        LogPrintf("ThreadStakeMinter: Coinstake output will mature in %d blocks\n",
                                 COINBASE_MATURITY);
                        LogPrintf("ThreadStakeMinter: Masternode will automatically re-register after maturity\n");
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
