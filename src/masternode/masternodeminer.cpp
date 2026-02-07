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
    if (!pmn || !pmn->IsEnabled()) {
        LogPrintf("CMasternodeMiner: No active masternode registered for IP %s. Run 'masternode start' to activate.\n", myAddr.ToString());
        return false;
    }

    // Verify we own the registered masternode's collateral
    const CWalletTx* wtx = pwallet->GetWalletTx(pmn->outpoint.hash);
    if (!wtx || pmn->outpoint.n >= wtx->tx->vout.size() || !pwallet->IsMine(wtx->tx->vout[pmn->outpoint.n])) {
        LogPrintf("CMasternodeMiner: Registered masternode collateral %s not found in our wallet\n", pmn->outpoint.ToString());
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

    // Check minimum confirmations
    int nDepth = chainActive.Height() - coin.nHeight + 1;
    if (nDepth < params.nMasternodeMinimumConfirmations) {
        LogPrintf("CMasternodeMiner: Collateral %s needs %d more confirmations (has %d)\n",
                 pmn->outpoint.ToString(),
                 params.nMasternodeMinimumConfirmations - nDepth,
                 nDepth);
        return false;
    }

    // Build a COutput for the collateral — the UTXO is locked, never spent
    LogPrintf("CMasternodeMiner: Using locked collateral %s at %s\n",
             pmn->outpoint.ToString(), pmn->addr.ToString());

    // Find this output in the wallet's available coins to get a proper COutput
    std::vector<COutput> vAvailableCoins;
    pwallet->AvailableCoins(vAvailableCoins, true, nullptr, 1, MAX_MONEY, MAX_MONEY, 0, 0, 9999999);

    for (const COutput& out : vAvailableCoins) {
        COutPoint outpoint(out.tx->GetHash(), out.i);
        if (outpoint == pmn->outpoint) {
            vCoins.push_back(out);
            return true;
        }
    }

    LogPrintf("CMasternodeMiner: Collateral UTXO %s exists but not available in wallet (may be immature)\n",
             pmn->outpoint.ToString());
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

        // Try different timestamps (current time + up to 30 seconds into future)
        int64_t nCurrentTime = GetAdjustedTime();

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
                // Found valid stake! Build the proof transaction.
                // The collateral is NOT spent — the input reference + signature
                // proves ownership of the locked collateral UTXO.
                CMutableTransaction txNewMut;
                nTxNewTime = nTryTime;

                // Input: reference the collateral outpoint (proves ownership, NOT spent)
                txNewMut.vin.resize(1);
                txNewMut.vin[0].prevout = prevout;

                // Calculate block reward
                CAmount nReward = GetBlockSubsidy(pindexPrev->nHeight + 1, params);

                // Output 0: empty marker (keeps IsCoinStake() = true for block identification)
                // Output 1: full block reward to the masternode operator's address
                txNewMut.vout.resize(2);
                txNewMut.vout[0].SetEmpty();
                txNewMut.vout[1].nValue = nReward;
                txNewMut.vout[1].scriptPubKey = wtx->tx->vout[coin.i].scriptPubKey;

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

                if (ProcessNewBlock(Params(), shared_pblock, true, &fNewBlock)) {
                    if (fNewBlock) {
                        LogPrintf("ThreadStakeMinter: Masternode block accepted! Height=%d Hash=%s\n",
                                 chainActive.Height(), block.GetHash().ToString());
                        LogPrintf("ThreadStakeMinter: Collateral remains locked, reward in proof tx output\n");
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
