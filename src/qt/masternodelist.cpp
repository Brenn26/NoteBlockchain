// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodelist.h"
#include <qt/forms/ui_masternodelist.h>

#include "clientmodel.h"
#include "walletmodel.h"
#include "masternode/masternode.h"
#include "masternode/masternodeman.h"
#include "masternode/masternodeminer.h"
#include "guiutil.h"
#include "validation.h"
#include "wallet/wallet.h"
#include "primitives/transaction.h"
#include "consensus/consensus.h"
#include "utilmoneystr.h"
#include "keystore.h"
#include "chainparams.h"
#include "netbase.h"
#include "util.h"
#include "net.h"
#include "netmessagemaker.h"

#include <QTimer>
#include <QMessageBox>

MasternodeList::MasternodeList(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MasternodeList),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);

    // Setup timer for updating the list
    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMyNodeList()));
    connect(timer, SIGNAL(timeout()), this, SLOT(updateNetworkList()));
    timer->start(30000); // Update every 30 seconds

    // Initial update
    updateMyNodeList();
    updateNetworkList();
}

MasternodeList::~MasternodeList()
{
    delete ui;
}

void MasternodeList::setClientModel(ClientModel *model)
{
    this->clientModel = model;
}

void MasternodeList::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if (model) {
        updateMyNodeList();
    }
}

void MasternodeList::updateMyNodeList()
{
    if (!walletModel || !walletModel->getWallet())
        return;

    CWallet* pwallet = walletModel->getWallet();

    // Clear current info
    ui->myMasternodeStatus->setText("Not Running");
    ui->myMasternodeAddress->setText("N/A");
    ui->myExpectedReward->setText("0.00 NOTE");
    ui->myBlocksProduced->setText("0");
    ui->myTotalRewards->setText("0.00 NOTE");
    ui->myEstimatedTime->setText("N/A");

    // Check if we have a masternode running
    bool foundMasternode = false;
    CAmount totalRewards = 0;

    // Search ALL wallet transactions for collateral (including locked UTXOs)
    const Consensus::Params& params = Params().GetConsensus();

    for (const auto& pair : pwallet->mapWallet) {
        if (foundMasternode) break;
        const CWalletTx& wtx = pair.second;
        if (wtx.GetDepthInMainChain() <= 0)
            continue;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            if (wtx.tx->vout[i].nValue != params.nMasternodeCollateral)
                continue;
            if (!pwallet->IsMine(wtx.tx->vout[i]))
                continue;

            COutPoint outpoint(wtx.tx->GetHash(), i);

            // Check if this is registered as a masternode
            CMasternode* pmn = mnodeman.Find(outpoint);
            if (pmn && pmn->IsEnabled()) {
                foundMasternode = true;

                // Update status
                ui->myMasternodeStatus->setText(QString::fromStdString(pmn->GetStatus()));
                ui->myMasternodeAddress->setText(QString::fromStdString(pmn->addr.ToString()));

                // Calculate expected reward per block (full block reward)
                CAmount blockReward = GetBlockSubsidy(chainActive.Height() + 1, Params().GetConsensus());
                ui->myExpectedReward->setText(QString::fromStdString(FormatMoney(blockReward)));

                // Calculate total rewards earned from masternode blocks
                // Proof tx structure: vout[0]=empty marker, vout[1]=reward
                totalRewards = 0;
                int blocksProduced = 0;
                for (const auto& entry : pwallet->mapWallet) {
                    const CWalletTx& rewardWtx = entry.second;
                    if (rewardWtx.tx->IsCoinStake() && rewardWtx.GetDepthInMainChain() > 0) {
                        if (rewardWtx.tx->vout.size() >= 2 && pwallet->IsMine(rewardWtx.tx->vout[1])) {
                            CAmount nReward = rewardWtx.tx->vout[1].nValue;
                            if (nReward > 0) {
                                totalRewards += nReward;
                                blocksProduced++;
                            }
                        }
                    }
                }
                ui->myBlocksProduced->setText(QString::number(blocksProduced));
                ui->myTotalRewards->setText(QString::fromStdString(FormatMoney(totalRewards) + " NOTE"));

                // Estimated time until next reward
                int nEnabledMasternodes = mnodeman.CountEnabled();
                if (nEnabledMasternodes <= 0) nEnabledMasternodes = 1;
                int64_t nEstimatedSeconds = 45 * nEnabledMasternodes; // 45s PoS target * number of MNs
                int hours = nEstimatedSeconds / 3600;
                int minutes = (nEstimatedSeconds % 3600) / 60;
                int seconds = nEstimatedSeconds % 60;
                QString timeStr;
                if (hours > 0) timeStr += QString::number(hours) + "h ";
                if (minutes > 0) timeStr += QString::number(minutes) + "m ";
                timeStr += QString::number(seconds) + "s";
                ui->myEstimatedTime->setText(timeStr);

                break;
            }
        }
    }

    if (!foundMasternode) {
        ui->myMasternodeStatus->setText("Not Running");
    }
}

void MasternodeList::updateNetworkList()
{
    if (!clientModel)
        return;

    // Update network statistics - only show active masternodes
    int enabledMasternodes = mnodeman.CountEnabled();
    ui->networkMasternodeCount->setText(QString::number(enabledMasternodes));

    // Calculate next masternode payment
    int nBlockHeight = chainActive.Height();
    CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(nBlockHeight + 1);

    if (pmn) {
        ui->nextPayee->setText(QString::fromStdString(pmn->addr.ToString()));
    } else {
        ui->nextPayee->setText("N/A");
    }
}

void MasternodeList::on_startButton_clicked()
{
    if (!walletModel || !walletModel->getWallet())
        return;

    CWallet* pwallet = walletModel->getWallet();

    // Check if wallet is locked
    if (pwallet->IsLocked()) {
        QMessageBox::warning(this, tr("Wallet Locked"),
            tr("Please unlock your wallet to start masternode."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Clean up any masternodes with spent UTXOs first
    // This prevents double-counting when re-registering after restaking
    mnodeman.CheckAndRemove();

    // Find collateral — search ALL wallet transactions (including locked UTXOs)
    const Consensus::Params& params = Params().GetConsensus();
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
        QString collateralAmount = QString::fromStdString(FormatMoney(params.nMasternodeCollateral));
        QMessageBox::warning(this, tr("No Collateral"),
            tr("No masternode collateral (%1) found in your wallet.").arg(collateralAmount),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Check if already registered
    if (mnodeman.Has(foundOutpoint)) {
        QMessageBox::information(this, tr("Already Running"),
            tr("This masternode is already registered and running."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Get the address for this output
    CTxDestination dest;
    if (!ExtractDestination(foundWtx->tx->vout[foundVout].scriptPubKey, dest)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to extract destination from collateral output."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Get the public key and private key
    CKeyID keyID = GetKeyForDestination(*pwallet, dest);
    CPubKey pubKey;
    if (!pwallet->GetPubKey(keyID, pubKey)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to get public key for collateral address."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    CKey key;
    if (!pwallet->GetKey(keyID, key)) {
        QMessageBox::warning(this, tr("Wallet Error"),
            tr("Failed to get private key for signing masternode announcement"),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Get external IP from config file
    CService addr;
    std::string externalIP = gArgs.GetArg("-externalip", "");
    if (externalIP.empty()) {
        QMessageBox::warning(this, tr("Configuration Error"),
            tr("No external IP configured. Please add 'externalip=YOUR.IP.ADDRESS' to noteblockchain.conf"),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    if (!Lookup(externalIP.c_str(), addr, Params().GetDefaultPort(), false)) {
        QMessageBox::warning(this, tr("Configuration Error"),
            tr("Failed to lookup external IP address: ") + QString::fromStdString(externalIP),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Check if this IP is already registered
    if (mnodeman.HasIP(addr)) {
        CMasternode* existingMN = mnodeman.FindByIP(addr);
        if (existingMN) {
            mnodeman.Remove(existingMN->outpoint);
            LogPrintf("Masternode: Updating %s with new collateral %s (old: %s)\n",
                     addr.ToString(), foundOutpoint.ToString(), existingMN->outpoint.ToString());
        }
    }

    CMasternode mn(foundOutpoint, addr, pubKey);

    // Sign the masternode announcement
    if (!mn.Sign(key)) {
        QMessageBox::warning(this, tr("Signing Error"),
            tr("Failed to sign masternode announcement"),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    if (mnodeman.Add(mn)) {
        // Lock the collateral UTXO — it is never spent
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

        QMessageBox::information(this, tr("Success"),
            tr("Masternode started, collateral locked!"),
            QMessageBox::Ok, QMessageBox::Ok);
        updateMyNodeList();
        return;
    }
}

void MasternodeList::on_stopButton_clicked()
{
    if (!walletModel || !walletModel->getWallet())
        return;

    CWallet* pwallet = walletModel->getWallet();

    // Find our masternode by IP
    std::string externalIP = gArgs.GetArg("-externalip", "");
    if (externalIP.empty()) {
        QMessageBox::warning(this, tr("Configuration Error"),
            tr("No external IP configured."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    CService addr;
    if (!Lookup(externalIP.c_str(), addr, Params().GetDefaultPort(), false)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to lookup external IP address."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    CMasternode* pmn = mnodeman.FindByIP(addr);
    if (!pmn) {
        QMessageBox::information(this, tr("Not Running"),
            tr("No active masternode found for this IP address."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Verify we own this masternode's collateral
    const CWalletTx* wtx = pwallet->GetWalletTx(pmn->outpoint.hash);
    if (!wtx || pmn->outpoint.n >= wtx->tx->vout.size() ||
        !pwallet->IsMine(wtx->tx->vout[pmn->outpoint.n])) {
        QMessageBox::warning(this, tr("Error"),
            tr("Masternode collateral not owned by this wallet."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    COutPoint removedOutpoint = pmn->outpoint;

    // Unlock the collateral
    pwallet->UnlockCoin(removedOutpoint);

    // Remove from masternode list
    mnodeman.Remove(removedOutpoint);

    // Disable staking
    masternodeMiner.DisableStaking();

    QMessageBox::information(this, tr("Stopped"),
        tr("Masternode stopped, collateral unlocked."),
        QMessageBox::Ok, QMessageBox::Ok);

    updateMyNodeList();
}
