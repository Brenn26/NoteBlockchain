// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodelist.h"
#include "ui_masternodelist.h"

#include "clientmodel.h"
#include "walletmodel.h"
#include "masternode/masternode.h"
#include "masternode/masternodeman.h"
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
    ui->myTotalRewards->setText("0.00 NOTE");

    // Check if we have a masternode running
    bool foundMasternode = false;
    CAmount totalRewards = 0;

    // Iterate through our wallet outputs to find collateral
    std::vector<COutput> vCoins;
    pwallet->AvailableCoins(vCoins);

    for (const auto& out : vCoins) {
        if (out.tx->tx->vout[out.i].nValue == Params().GetConsensus().nMasternodeCollateral) {
            // Found potential collateral
            COutPoint outpoint(out.tx->tx->GetHash(), out.i);

            // Check if this is registered as a masternode
            CMasternode* pmn = mnodeman.Find(outpoint);
            if (pmn && pmn->IsEnabled()) {
                foundMasternode = true;

                // Update status
                ui->myMasternodeStatus->setText(QString::fromStdString(pmn->GetStatus()));
                ui->myMasternodeAddress->setText(QString::fromStdString(pmn->addr.ToString()));

                // Calculate expected reward per stake (full PoS block reward)
                CAmount blockReward = GetBlockSubsidy(chainActive.Height() + 1, Params().GetConsensus());
                CAmount masternodeReward = blockReward;  // Masternode gets full reward when staking
                ui->myExpectedReward->setText(QString::fromStdString(FormatMoney(masternodeReward)));

                // Calculate total rewards earned from PoS blocks
                totalRewards = 0;
                int blocksStaked = 0;
                for (const auto& entry : pwallet->mapWallet) {
                    const CWalletTx& wtx = entry.second;
                    if (wtx.tx->IsCoinStake() && wtx.GetDepthInMainChain() > 0) {
                        // Calculate actual reward (output - input)
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
                            totalRewards += nReward;
                            blocksStaked++;
                        }
                    }
                }
                ui->myTotalRewards->setText(QString::fromStdString(FormatMoney(totalRewards) + " (" + std::to_string(blocksStaked) + " blocks)"));

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

    // Try to find collateral and start masternode
    std::vector<COutput> vCoins;
    pwallet->AvailableCoins(vCoins);

    bool foundCollateral = false;
    for (const auto& out : vCoins) {
        if (out.tx->tx->vout[out.i].nValue == Params().GetConsensus().nMasternodeCollateral) {
            foundCollateral = true;

            COutPoint outpoint(out.tx->tx->GetHash(), out.i);

            // Check if already registered
            if (mnodeman.Has(outpoint)) {
                QMessageBox::information(this, tr("Already Running"),
                    tr("This masternode is already registered and running."),
                    QMessageBox::Ok, QMessageBox::Ok);
                return;
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

            // Create masternode entry
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
            // If so, remove the old entry (with spent outpoint) and update with new one
            if (mnodeman.HasIP(addr)) {
                CMasternode* existingMN = mnodeman.FindByIP(addr);
                if (existingMN) {
                    // Remove the old entry
                    mnodeman.Remove(existingMN->outpoint);
                    LogPrintf("Masternode: Updating %s with new collateral %s (old: %s)\n",
                             addr.ToString(), outpoint.ToString(), existingMN->outpoint.ToString());
                }
            }

            CMasternode mn(outpoint, addr, pubKey);

            if (mnodeman.Add(mn)) {
                // Broadcast masternode to network
                if (g_connman) {
                    g_connman->ForEachNode([&mn](CNode* pnode) {
                        g_connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::MNANNOUNCE, mn));
                    });
                    LogPrintf("Broadcast masternode %s to network\n", mn.outpoint.ToString());
                }

                QMessageBox::information(this, tr("Success"),
                    tr("Masternode started successfully!"),
                    QMessageBox::Ok, QMessageBox::Ok);
                updateMyNodeList();
                return;
            }

            break;
        }
    }

    if (!foundCollateral) {
        QString collateralAmount = QString::fromStdString(FormatMoney(Params().GetConsensus().nMasternodeCollateral));
        QMessageBox::warning(this, tr("No Collateral"),
            tr("No masternode collateral (%1) found in your wallet.").arg(collateralAmount),
            QMessageBox::Ok, QMessageBox::Ok);
    }
}

void MasternodeList::on_startAllButton_clicked()
{
    // This would start all available masternodes
    // For now, just call the single start function
    on_startButton_clicked();
}
