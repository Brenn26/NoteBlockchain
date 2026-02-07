// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/server.h"
#include "masternode/masternode.h"
#include "masternode/masternodeman.h"
#include "masternode/masternodeminer.h"
#include "chainparams.h"
#include "validation.h"
#include "wallet/wallet.h"
#include "wallet/rpcwallet.h"
#include "util.h"
#include "utilmoneystr.h"  // For FormatMoney()
#include "core_io.h"       // For ValueFromAmount()
#include "netbase.h"       // For Lookup()
#include "net.h"           // For g_connman
#include "netmessagemaker.h" // For CNetMsgMaker



UniValue masternode(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    if (request.fHelp ||
        (strCommand != "count" && strCommand != "list" &&
         strCommand != "status" && strCommand != "start" && strCommand != "stop" && strCommand != "help"))
        throw std::runtime_error(
            "masternode \"command\"...\n"
            "Set of commands to execute masternode related actions\n"
            "\nArguments:\n"
            "1. \"command\"        (string, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  count        - Get network total and user's masternode count\n"
            "  list         - List all masternodes on the network\n"
            "  status       - Get masternode status (network and user's)\n"
            "  start        - Start your masternode (locks collateral)\n"
            "  stop         - Stop your masternode (unlocks collateral)\n"
            "  help         - Show detailed setup instructions\n"
            "\nExamples:\n"
            + HelpExampleCli("masternode", "count")
            + HelpExampleCli("masternode", "start")
            + HelpExampleCli("masternode", "stop")
            + HelpExampleCli("masternode", "help")
            + HelpExampleRpc("masternode", "\"status\"")
        );

    if (strCommand == "count") {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("network_total", mnodeman.CountEnabled());  // Only count active masternodes
        obj.pushKV("network_enabled", mnodeman.CountEnabled());

        // Count user's masternodes
        CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
        if (pwallet && EnsureWalletIsAvailable(pwallet, false)) {
            LOCK2(cs_main, pwallet->cs_wallet);
            int nMyMasternodes = 0;
            std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
            for (const auto& mn : vMasternodes) {
                // Check if we own this masternode's collateral
                const CWalletTx* wtx = pwallet->GetWalletTx(mn.outpoint.hash);
                if (wtx && mn.outpoint.n < wtx->tx->vout.size()) {
                    if (pwallet->IsMine(wtx->tx->vout[mn.outpoint.n])) {
                        nMyMasternodes++;
                    }
                }
            }
            obj.pushKV("my_masternodes", nMyMasternodes);
        }

        return obj;
    }

    if (strCommand == "list") {
        UniValue obj(UniValue::VOBJ);
        std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();

        for (const auto& mn : vMasternodes) {
            UniValue mnObj(UniValue::VOBJ);
            mnObj.pushKV("address", mn.addr.ToString());
            mnObj.pushKV("status", mn.GetStatus());
            mnObj.pushKV("protocol", mn.nProtocolVersion);
            mnObj.pushKV("lastseen", mn.nTimeLastSeen);
            mnObj.pushKV("collateral", mn.outpoint.ToString());

            obj.pushKV(mn.outpoint.hash.ToString(), mnObj);
        }
        return obj;
    }

    if (strCommand == "status") {
        CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
        if (!EnsureWalletIsAvailable(pwallet, false))
            return NullUniValue;

        LOCK2(cs_main, pwallet->cs_wallet);

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("staking_enabled", masternodeMiner.CanStake(Params()));
        obj.pushKV("masternodes_network_total", mnodeman.CountEnabled());  // Only count active masternodes
        obj.pushKV("masternodes_network_enabled", mnodeman.CountEnabled());

        // Count user's masternodes
        int nMyMasternodes = 0;
        int nMyMasternodesEnabled = 0;
        std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
        for (const auto& mn : vMasternodes) {
            // Check if we own this masternode's collateral
            const CWalletTx* wtx = pwallet->GetWalletTx(mn.outpoint.hash);
            if (wtx && mn.outpoint.n < wtx->tx->vout.size()) {
                if (pwallet->IsMine(wtx->tx->vout[mn.outpoint.n])) {
                    nMyMasternodes++;
                    if (mn.GetStatus() == "ENABLED") {
                        nMyMasternodesEnabled++;
                    }
                }
            }
        }
        obj.pushKV("my_masternodes", nMyMasternodes);
        obj.pushKV("my_masternodes_enabled", nMyMasternodesEnabled);

        obj.pushKV("block_height", chainActive.Height());

        const Consensus::Params& params = Params().GetConsensus();
        obj.pushKV("pos_activated", chainActive.Height() >= params.nMasternodeActivationHeight);
        obj.pushKV("collateral_required", ValueFromAmount(params.nMasternodeCollateral));

        // Reward tracking for this wallet's masternodes
        CAmount nTotalRewards = 0;
        int nBlocksProduced = 0;
        for (const auto& pair : pwallet->mapWallet) {
            const CWalletTx& wtx = pair.second;
            if (wtx.tx->IsCoinStake() && wtx.GetDepthInMainChain() > 0) {
                if (wtx.tx->vout.size() >= 2 && pwallet->IsMine(wtx.tx->vout[1])) {
                    CAmount nReward = wtx.tx->vout[1].nValue;
                    if (nReward > 0) {
                        nTotalRewards += nReward;
                        nBlocksProduced++;
                    }
                }
            }
        }
        obj.pushKV("blocks_produced", nBlocksProduced);
        obj.pushKV("total_rewards", ValueFromAmount(nTotalRewards));
        obj.pushKV("total_rewards_formatted", FormatMoney(nTotalRewards) + " NOTE");

        // Estimated time until next reward
        int64_t nExpectedTime = masternodeMiner.GetExpectedStakeTime(Params(), pwallet);
        if (nExpectedTime > 0) {
            obj.pushKV("estimated_time_to_next_reward_seconds", nExpectedTime);
            // Human-readable format
            int hours = nExpectedTime / 3600;
            int minutes = (nExpectedTime % 3600) / 60;
            int seconds = nExpectedTime % 60;
            std::string timeStr;
            if (hours > 0) timeStr += std::to_string(hours) + "h ";
            if (minutes > 0) timeStr += std::to_string(minutes) + "m ";
            timeStr += std::to_string(seconds) + "s";
            obj.pushKV("estimated_time_to_next_reward", timeStr);
        } else {
            obj.pushKV("estimated_time_to_next_reward", "N/A (masternode not staking)");
        }

        return obj;
    }

    if (strCommand == "start") {
        CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
        if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
            return NullUniValue;

        if (pwallet->IsLocked())
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please unlock wallet to start masternode.");

        LOCK2(cs_main, pwallet->cs_wallet);

        const Consensus::Params& params = Params().GetConsensus();

        // Clean up any masternodes with spent UTXOs first
        // This prevents double-counting when re-registering after restaking
        mnodeman.CheckAndRemove();

        // Find collateral — search ALL wallet transactions (including locked UTXOs)
        // AvailableCoins skips locked UTXOs, but we need to find them for re-registration
        COutPoint foundOutpoint;
        const CWalletTx* foundWtx = nullptr;
        int foundVout = -1;

        for (const auto& pair : pwallet->mapWallet) {
            const CWalletTx& wtx = pair.second;
            if (wtx.GetDepthInMainChain() <= 0)
                continue;
            for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
                if (wtx.tx->vout[i].nValue == params.nMasternodeCollateral &&
                    pwallet->IsMine(wtx.tx->vout[i])) {
                    COutPoint outpoint(wtx.tx->GetHash(), i);

                    // Verify the UTXO is still unspent in the chain
                    Coin coin;
                    if (!pcoinsTip || !pcoinsTip->GetCoin(outpoint, coin) || coin.IsSpent())
                        continue;

                    foundOutpoint = outpoint;
                    foundWtx = &wtx;
                    foundVout = i;
                    break;
                }
            }
            if (foundVout >= 0) break;
        }

        if (foundVout < 0) {
            throw JSONRPCError(RPC_MISC_ERROR,
                "No masternode collateral (" + FormatMoney(params.nMasternodeCollateral) + ") found in wallet");
        }

        {
            // Check if already registered
            if (mnodeman.Has(foundOutpoint)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Masternode already registered");
            }

            // Get the address for this output
            CTxDestination dest;
            if (!ExtractDestination(foundWtx->tx->vout[foundVout].scriptPubKey, dest)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Failed to extract destination from collateral output");
            }

            // Get the public key
            CKeyID keyID = GetKeyForDestination(*pwallet, dest);
            CPubKey pubKey;
            if (!pwallet->GetPubKey(keyID, pubKey)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to get public key for collateral address");
            }

            // Get the private key for signing
            CKey key;
            if (!pwallet->GetKey(keyID, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to get private key for signing masternode announcement");
            }

            // Get external IP address from config
            CService addr;
            std::string externalIP = gArgs.GetArg("-externalip", "");
            if (externalIP.empty()) {
                throw JSONRPCError(RPC_MISC_ERROR,
                    "No external IP configured. Add 'externalip=YOUR.IP.ADDRESS' to noteblockchain.conf");
            }

            if (!Lookup(externalIP.c_str(), addr, Params().GetDefaultPort(), false)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Failed to lookup external IP address: " + externalIP);
            }

            // Check if this IP is already registered
            // If so, remove the old entry and update with new one
            if (mnodeman.HasIP(addr)) {
                CMasternode* existingMN = mnodeman.FindByIP(addr);
                if (existingMN) {
                    mnodeman.Remove(existingMN->outpoint);
                    LogPrintf("Masternode: Updating %s with new collateral %s (old: %s)\n",
                             addr.ToString(), foundOutpoint.ToString(), existingMN->outpoint.ToString());
                }
            }

            // Create and sign masternode
            CMasternode mn(foundOutpoint, addr, pubKey);
            if (!mn.Sign(key)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Failed to sign masternode announcement");
            }

            if (mnodeman.Add(mn)) {
                // Lock the collateral UTXO to prevent accidental spending
                // The collateral remains in the UTXO set — it is never spent
                pwallet->LockCoin(foundOutpoint);

                // Enable staking now that masternode is registered
                masternodeMiner.EnableStaking();

                // Broadcast masternode to network
                if (g_connman) {
                    g_connman->ForEachNode([&mn](CNode* pnode) {
                        g_connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::MNANNOUNCE, mn));
                    });
                    LogPrintf("Broadcast masternode %s to network\n", mn.outpoint.ToString());
                }

                UniValue obj(UniValue::VOBJ);
                obj.pushKV("status", "success");
                obj.pushKV("message", "Masternode started, collateral locked");
                obj.pushKV("address", addr.ToString());
                obj.pushKV("collateral", foundOutpoint.ToString());
                return obj;
            }

            throw JSONRPCError(RPC_MISC_ERROR, "Failed to add masternode");
        }
    }

    if (strCommand == "stop") {
        CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
        if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
            return NullUniValue;

        LOCK2(cs_main, pwallet->cs_wallet);

        const Consensus::Params& params = Params().GetConsensus();
        UniValue obj(UniValue::VOBJ);
        bool stoppedMasternode = false;

        // Try to find registered masternode by IP first
        std::string externalIP = gArgs.GetArg("-externalip", "");
        if (!externalIP.empty()) {
            CService addr;
            if (Lookup(externalIP.c_str(), addr, Params().GetDefaultPort(), false)) {
                CMasternode* pmn = mnodeman.FindByIP(addr);
                if (pmn) {
                    COutPoint removedOutpoint = pmn->outpoint;
                    pwallet->UnlockCoin(removedOutpoint);
                    mnodeman.Remove(removedOutpoint);
                    obj.pushKV("collateral", removedOutpoint.ToString());
                    stoppedMasternode = true;
                }
            }
        }

        // Also unlock any locked collateral-sized UTXOs that may be orphaned
        // (e.g., from a previous session where the masternode registration was lost)
        std::set<COutPoint> lockedCoins;
        pwallet->ListLockedCoins(lockedCoins);
        int nUnlocked = 0;
        for (const COutPoint& outpoint : lockedCoins) {
            const CWalletTx* wtx = pwallet->GetWalletTx(outpoint.hash);
            if (wtx && outpoint.n < wtx->tx->vout.size() &&
                wtx->tx->vout[outpoint.n].nValue == params.nMasternodeCollateral) {
                pwallet->UnlockCoin(outpoint);
                nUnlocked++;
                if (!stoppedMasternode) {
                    obj.pushKV("collateral", outpoint.ToString());
                }
            }
        }

        // Disable staking
        masternodeMiner.DisableStaking();

        if (stoppedMasternode || nUnlocked > 0) {
            obj.pushKV("status", "success");
            if (stoppedMasternode)
                obj.pushKV("message", "Masternode stopped, collateral unlocked");
            else
                obj.pushKV("message", "No registered masternode found, but unlocked " + std::to_string(nUnlocked) + " locked collateral UTXO(s)");
        } else {
            throw JSONRPCError(RPC_MISC_ERROR, "No active masternode or locked collateral found");
        }

        return obj;
    }

    if (strCommand == "help") {
        const Consensus::Params& params = Params().GetConsensus();

        std::string helpText =
            "===========================================\n"
            "  MASTERNODE SETUP GUIDE\n"
            "===========================================\n\n"

            "OVERVIEW:\n"
            "---------\n"
            "Masternodes lock collateral and produce blocks that compete with\n"
            "PoW miners. The collateral is NEVER spent - it stays locked in\n"
            "your wallet as proof of commitment. Whoever finds a block (miner\n"
            "or masternode) receives the full block reward.\n\n"

            "REQUIREMENTS:\n"
            "-------------\n"
            "1. Collateral: " + FormatMoney(params.nMasternodeCollateral) + " coins (exact amount in single UTXO)\n"
            "2. Public IP address (unique per masternode)\n"
            "3. Minimum " + std::to_string(params.nMasternodeMinimumConfirmations) + " confirmations on collateral\n"
            "4. Minimum coin age: " + std::to_string(params.nStakeMinAge / 3600) + " hour(s)\n\n"

            "SETUP STEPS:\n"
            "------------\n"
            "1. Send exactly " + FormatMoney(params.nMasternodeCollateral) + " coins to your wallet\n\n"

            "2. Configure your noteblockchain.conf file:\n\n"
            "   # Set your public IP address\n"
            "   # Find your IP at: https://whatismyipaddress.com\n"
            "   externalip=YOUR.PUBLIC.IP.HERE\n\n"

            "   # RPC settings (for monitoring)\n"
            "   server=1\n"
            "   rpcuser=yourusername\n"
            "   rpcpassword=yourpassword\n\n"

            "3. Restart your wallet/node\n\n"

            "4. Wait for " + std::to_string(params.nMasternodeMinimumConfirmations) + " confirmations (~" +
                std::to_string(params.nMasternodeMinimumConfirmations * 30 / 60) + " minutes)\n\n"

            "5. Start your masternode (locks collateral and begins block production):\n"
            "   notecoin-cli masternode start\n\n"

            "6. Check status:\n"
            "   notecoin-cli masternode status\n"
            "   notecoin-cli getstakingstatus\n\n"

            "7. To stop your masternode and unlock collateral:\n"
            "   notecoin-cli masternode stop\n\n"

            "IMPORTANT NOTES:\n"
            "----------------\n"
            "* ONE masternode per PUBLIC IP address\n"
            "* Private IPs (192.168.x.x, 10.x.x.x) are NOT allowed\n"
            "* Collateral is LOCKED, not spent - it remains in your wallet\n"
            "* If collateral is spent (e.g. via raw transaction), masternode is removed\n"
            "* Use 'masternode stop' to unlock collateral before spending\n\n"

            "MULTIPLE MASTERNODES:\n"
            "---------------------\n"
            "To run multiple masternodes:\n"
            "1. Rent separate VPS servers (different public IPs)\n"
            "2. Install wallet on each server\n"
            "3. Send " + FormatMoney(params.nMasternodeCollateral) + " coins to each\n"
            "4. Configure each with its unique externalip\n\n"

            "TROUBLESHOOTING:\n"
            "----------------\n"
            "If masternode not producing blocks:\n"
            "* Check: notecoin-cli getstakingstatus\n"
            "* Verify external IP configured correctly\n"
            "* Ensure " + std::to_string(params.nMasternodeMinimumConfirmations) + "+ confirmations\n"
            "* Check debug.log for errors\n"
            "* Verify exactly " + FormatMoney(params.nMasternodeCollateral) + " in single UTXO\n\n"

            "For more help, visit: www.notebc.com\n"
            "===========================================\n";

        UniValue result(UniValue::VOBJ);
        result.pushKV("help", helpText);
        return result;
    }

    return NullUniValue;
}

UniValue getstakingstatus(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getstakingstatus\n"
            "Returns staking status and reward tracking information.\n"
            "\nResult:\n"
            "{\n"
            "  \"staking\": true|false,           (boolean) whether staking is enabled\n"
            "  \"enabled_masternodes\": n,        (numeric) number of enabled masternodes on network\n"
            "  \"block_height\": n,               (numeric) current block height\n"
            "  \"blocks_produced\": n,            (numeric) number of masternode blocks you produced\n"
            "  \"total_rewards\": x.xxx,          (numeric) total rewards earned\n"
            "  \"estimated_time_to_next_reward_seconds\": n, (numeric) estimated seconds until next reward\n"
            "  \"estimated_time_to_next_reward\": \"str\",   (string) human-readable time estimate\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getstakingstatus", "")
            + HelpExampleRpc("getstakingstatus", "")
        );

    CWallet* pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("staking", masternodeMiner.CanStake(Params()));
    obj.pushKV("enabled_masternodes", mnodeman.CountEnabled());
    obj.pushKV("block_height", chainActive.Height());

    // Reward tracking
    CAmount nTotalRewards = 0;
    int nBlocksProduced = 0;
    for (const auto& pair : pwallet->mapWallet) {
        const CWalletTx& wtx = pair.second;
        if (wtx.tx->IsCoinStake() && wtx.GetDepthInMainChain() > 0) {
            if (wtx.tx->vout.size() >= 2 && pwallet->IsMine(wtx.tx->vout[1])) {
                CAmount nReward = wtx.tx->vout[1].nValue;
                if (nReward > 0) {
                    nTotalRewards += nReward;
                    nBlocksProduced++;
                }
            }
        }
    }
    obj.pushKV("blocks_produced", nBlocksProduced);
    obj.pushKV("total_rewards", ValueFromAmount(nTotalRewards));
    obj.pushKV("total_rewards_formatted", FormatMoney(nTotalRewards) + " NOTE");

    // Estimated time until next reward
    int64_t nExpectedTime = masternodeMiner.GetExpectedStakeTime(Params(), pwallet);
    if (nExpectedTime > 0) {
        obj.pushKV("estimated_time_to_next_reward_seconds", nExpectedTime);
        int hours = nExpectedTime / 3600;
        int minutes = (nExpectedTime % 3600) / 60;
        int seconds = nExpectedTime % 60;
        std::string timeStr;
        if (hours > 0) timeStr += std::to_string(hours) + "h ";
        if (minutes > 0) timeStr += std::to_string(minutes) + "m ";
        timeStr += std::to_string(seconds) + "s";
        obj.pushKV("estimated_time_to_next_reward", timeStr);
    } else {
        obj.pushKV("estimated_time_to_next_reward_seconds", -1);
        obj.pushKV("estimated_time_to_next_reward", "N/A (masternode not staking)");
    }

    return obj;
}

UniValue getposrewards(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getposrewards\n"
            "Returns total rewards earned from masternode blocks and estimated time to next reward.\n"
            "\nResult:\n"
            "{\n"
            "  \"total_masternode_rewards\": x.xxx,  (numeric) total rewards from masternode blocks\n"
            "  \"masternode_blocks\": n,             (numeric) number of masternode blocks produced\n"
            "  \"total_rewards_formatted\": \"x.xxx NOTE\", (string) formatted reward amount\n"
            "  \"reward_per_block\": x.xxx,          (numeric) current reward per block\n"
            "  \"enabled_masternodes\": n,            (numeric) number of enabled masternodes on network\n"
            "  \"estimated_time_to_next_reward_seconds\": n, (numeric) estimated seconds until next reward\n"
            "  \"estimated_time_to_next_reward\": \"str\",   (string) human-readable time estimate\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getposrewards", "")
            + HelpExampleRpc("getposrewards", "")
        );

    CWallet* pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    LOCK2(cs_main, pwallet->cs_wallet);

    CAmount nTotalRewards = 0;
    int nPosBlocks = 0;

    // Iterate through all wallet transactions
    for (const auto& pair : pwallet->mapWallet) {
        const CWalletTx& wtx = pair.second;

        // Check if this is a proof transaction (masternode block)
        // Proof tx structure: vout[0]=empty marker, vout[1]=reward
        // Collateral is never spent, so vout[1] is pure reward
        if (wtx.tx->IsCoinStake() && wtx.GetDepthInMainChain() > 0) {
            if (wtx.tx->vout.size() >= 2 && pwallet->IsMine(wtx.tx->vout[1])) {
                CAmount nReward = wtx.tx->vout[1].nValue;
                if (nReward > 0) {
                    nTotalRewards += nReward;
                    nPosBlocks++;
                }
            }
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("total_masternode_rewards", ValueFromAmount(nTotalRewards));
    obj.pushKV("masternode_blocks", nPosBlocks);
    obj.pushKV("total_rewards_formatted", FormatMoney(nTotalRewards) + " NOTE");

    // Current reward per block
    CAmount blockReward = GetBlockSubsidy(chainActive.Height() + 1, Params().GetConsensus());
    obj.pushKV("reward_per_block", ValueFromAmount(blockReward));
    obj.pushKV("reward_per_block_formatted", FormatMoney(blockReward) + " NOTE");

    // Network masternode count
    int nEnabledMasternodes = mnodeman.CountEnabled();
    obj.pushKV("enabled_masternodes", nEnabledMasternodes);

    // Estimated time until next reward
    int64_t nExpectedTime = masternodeMiner.GetExpectedStakeTime(Params(), pwallet);
    if (nExpectedTime > 0) {
        obj.pushKV("estimated_time_to_next_reward_seconds", nExpectedTime);
        int hours = nExpectedTime / 3600;
        int minutes = (nExpectedTime % 3600) / 60;
        int seconds = nExpectedTime % 60;
        std::string timeStr;
        if (hours > 0) timeStr += std::to_string(hours) + "h ";
        if (minutes > 0) timeStr += std::to_string(minutes) + "m ";
        timeStr += std::to_string(seconds) + "s";
        obj.pushKV("estimated_time_to_next_reward", timeStr);
    } else {
        obj.pushKV("estimated_time_to_next_reward_seconds", -1);
        obj.pushKV("estimated_time_to_next_reward", "N/A (masternode not staking)");
    }

    return obj;
}

static const CRPCCommand commands[] =
{ //  category           name                      actor (function)         argNames
  //  ------------------ ------------------------  -----------------------  ----------
    { "masternode",      "masternode",             &masternode,             {} },
    { "masternode",      "getstakingstatus",       &getstakingstatus,       {} },
    { "masternode",      "getposrewards",          &getposrewards,          {} },
};

void RegisterMasternodeRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
