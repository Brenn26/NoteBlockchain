// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/server.h"
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



UniValue masternode(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    if (request.fHelp ||
        (strCommand != "count" && strCommand != "list" &&
         strCommand != "status" && strCommand != "start" && strCommand != "help"))
        throw std::runtime_error(
            "masternode \"command\"...\n"
            "Set of commands to execute masternode related actions\n"
            "\nArguments:\n"
            "1. \"command\"        (string, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  count        - Get network total and user's masternode count\n"
            "  list         - List all masternodes on the network\n"
            "  status       - Get masternode status (network and user's)\n"
            "  start        - Start your masternode\n"
            "  help         - Show detailed setup instructions\n"
            "\nExamples:\n"
            + HelpExampleCli("masternode", "count")
            + HelpExampleCli("masternode", "start")
            + HelpExampleCli("masternode", "help")
            + HelpExampleRpc("masternode", "\"status\"")
        );

    if (strCommand == "count") {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("network_total", mnodeman.size());
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
        obj.pushKV("masternodes_network_total", mnodeman.size());
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

        // Find collateral
        std::vector<COutput> vCoins;
        pwallet->AvailableCoins(vCoins);

        for (const auto& out : vCoins) {
            if (out.tx->tx->vout[out.i].nValue == params.nMasternodeCollateral) {
                COutPoint outpoint(out.tx->tx->GetHash(), out.i);

                // Check if already registered
                if (mnodeman.Has(outpoint)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Masternode already registered");
                }

                // Get the address for this output
                CTxDestination dest;
                if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, dest)) {
                    continue;
                }

                // Get the public key
                CKeyID keyID = GetKeyForDestination(*pwallet, dest);
                CPubKey pubKey;
                if (!pwallet->GetPubKey(keyID, pubKey)) {
                    continue;
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

                // Create masternode
                CMasternode mn(outpoint, addr, pubKey);

                if (mnodeman.Add(mn)) {
                    UniValue obj(UniValue::VOBJ);
                    obj.pushKV("status", "success");
                    obj.pushKV("message", "Masternode started successfully");
                    obj.pushKV("address", addr.ToString());
                    obj.pushKV("collateral", outpoint.ToString());
                    return obj;
                }

                throw JSONRPCError(RPC_MISC_ERROR, "Failed to add masternode");
            }
        }

        throw JSONRPCError(RPC_MISC_ERROR,
            "No masternode collateral (" + FormatMoney(params.nMasternodeCollateral) + ") found in wallet");
    }

    if (strCommand == "help") {
        const Consensus::Params& params = Params().GetConsensus();

        std::string helpText =
            "===========================================\n"
            "  MASTERNODE SETUP GUIDE\n"
            "===========================================\n\n"

            "REQUIREMENTS:\n"
            "-------------\n"
            "1. Collateral: " + FormatMoney(params.nMasternodeCollateral) + " coins (exact amount)\n"
            "2. Public IP address (unique per masternode)\n"
            "3. Minimum " + std::to_string(params.nMasternodeMinimumConfirmations) + " confirmations\n"
            "4. Stake age: " + std::to_string(params.nStakeMinAge / 3600) + " hour(s)\n\n"

            "SETUP STEPS:\n"
            "------------\n"
            "1. Send exactly " + FormatMoney(params.nMasternodeCollateral) + " coins to your wallet\n\n"

            "2. Configure your notecoin.conf file:\n\n"
            "   # Set your public IP address\n"
            "   # Find your IP at: https://whatismyipaddress.com\n"
            "   externalip=YOUR.PUBLIC.IP.HERE\n\n"

            "   # Enable staking (default: enabled)\n"
            "   staking=1\n\n"

            "   # RPC settings (for monitoring)\n"
            "   server=1\n"
            "   rpcuser=yourusername\n"
            "   rpcpassword=yourpassword\n\n"

            "3. Restart your wallet/node\n\n"

            "4. Wait for " + std::to_string(params.nMasternodeMinimumConfirmations) + " confirmations (~" +
                std::to_string(params.nMasternodeMinimumConfirmations * 30 / 60) + " minutes)\n\n"

            "5. Check status:\n"
            "   notecoin-cli masternode status\n"
            "   notecoin-cli getstakingstatus\n\n"

            "IMPORTANT NOTES:\n"
            "----------------\n"
            "* ONE masternode per PUBLIC IP address\n"
            "* Private IPs (192.168.x.x, 10.x.x.x) are NOT allowed\n"
            "* For multiple masternodes, use different VPS/servers\n"
            "* Do NOT spend your " + FormatMoney(params.nMasternodeCollateral) + " collateral or staking stops\n"
            "* Coins automatically stake when eligible - no manual action needed\n\n"

            "MULTIPLE MASTERNODES:\n"
            "---------------------\n"
            "To run multiple masternodes:\n"
            "1. Rent separate VPS servers (different public IPs)\n"
            "2. Install wallet on each server\n"
            "3. Send " + FormatMoney(params.nMasternodeCollateral) + " coins to each\n"
            "4. Configure each with its unique externalip\n\n"

            "TROUBLESHOOTING:\n"
            "----------------\n"
            "If staking not working:\n"
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
            "Returns staking status information.\n"
            "\nResult:\n"
            "{\n"
            "  \"staking\": true|false,           (boolean) whether staking is enabled\n"
            "  \"expected_time\": n,              (numeric) estimated time until next stake (seconds)\n"
            "  \"masternode_count\": n,           (numeric) number of registered masternodes\n"
            "  \"block_height\": n,               (numeric) current block height\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getstakingstatus", "")
            + HelpExampleRpc("getstakingstatus", "")
        );

    CWallet* pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("staking", masternodeMiner.CanStake(Params()));

    int64_t nExpectedTime = masternodeMiner.GetExpectedStakeTime(Params(), pwallet);
    obj.pushKV("expected_time", nExpectedTime);
    obj.pushKV("masternode_count_network", mnodeman.size());
    obj.pushKV("masternode_count_enabled", mnodeman.CountEnabled());
    obj.pushKV("block_height", chainActive.Height());

    return obj;
}

UniValue getposrewards(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getposrewards\n"
            "Returns total rewards earned from Proof-of-Stake blocks.\n"
            "\nResult:\n"
            "{\n"
            "  \"total_pos_rewards\": x.xxx,      (numeric) total rewards from PoS blocks\n"
            "  \"pos_blocks_mined\": n,           (numeric) number of PoS blocks mined\n"
            "  \"total_pos_rewards_formatted\": \"x.xxx COIN\", (string) formatted reward amount\n"
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

        // Check if this is a coinstake transaction (PoS block)
        if (wtx.tx->IsCoinStake() && wtx.GetDepthInMainChain() > 0) {
            // Calculate the reward (output - input)
            CAmount nInput = 0;
            CAmount nOutput = 0;

            // Sum inputs
            for (const auto& txin : wtx.tx->vin) {
                const CWalletTx* prev = pwallet->GetWalletTx(txin.prevout.hash);
                if (prev && txin.prevout.n < prev->tx->vout.size()) {
                    nInput += prev->tx->vout[txin.prevout.n].nValue;
                }
            }

            // Sum outputs (skip first output which is empty in coinstake)
            for (size_t i = 1; i < wtx.tx->vout.size(); i++) {
                if (pwallet->IsMine(wtx.tx->vout[i])) {
                    nOutput += wtx.tx->vout[i].nValue;
                }
            }

            CAmount nReward = nOutput - nInput;
            if (nReward > 0) {
                nTotalRewards += nReward;
                nPosBlocks++;
            }
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("total_pos_rewards", ValueFromAmount(nTotalRewards));
    obj.pushKV("pos_blocks_mined", nPosBlocks);
    obj.pushKV("total_pos_rewards_formatted", FormatMoney(nTotalRewards) + " COIN");

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
