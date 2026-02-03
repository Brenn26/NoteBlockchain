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
        if (out.tx->tx->vout[out.i].nValue == 200000 * COIN) {
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

                // Calculate total rewards earned
                // This is a simplified calculation - in production you'd track this in the wallet
                int blocksStaked = 0;
                for (const auto& entry : pwallet->mapWallet) {
                    const CWalletTx& wtx = entry.second;
                    if (wtx.tx->IsCoinStake() && wtx.hashBlock != uint256()) {
                        blocksStaked++;
                    }
                }
                totalRewards = blocksStaked * masternodeReward;
                ui->myTotalRewards->setText(QString::fromStdString(FormatMoney(totalRewards)));

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

    // Update network statistics
    int totalMasternodes = mnodeman.CountEnabled();
    ui->networkMasternodeCount->setText(QString::number(totalMasternodes));

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

    // Try to find collateral and start masternode
    std::vector<COutput> vCoins;
    pwallet->AvailableCoins(vCoins);

    bool foundCollateral = false;
    for (const auto& out : vCoins) {
        if (out.tx->tx->vout[out.i].nValue == 200000 * COIN) {
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

            if (!Lookup(externalIP, addr, Params().GetDefaultPort(), false)) {
                QMessageBox::warning(this, tr("Configuration Error"),
                    tr("Failed to lookup external IP address: ") + QString::fromStdString(externalIP),
                    QMessageBox::Ok, QMessageBox::Ok);
                return;
            }

            CMasternode mn(outpoint, addr, pubKey);

            if (mnodeman.Add(mn)) {
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
        QMessageBox::warning(this, tr("No Collateral"),
            tr("No masternode collateral (200,000 NOTE) found in your wallet."),
            QMessageBox::Ok, QMessageBox::Ok);
    }
}

void MasternodeList::on_startAllButton_clicked()
{
    // This would start all available masternodes
    // For now, just call the single start function
    on_startButton_clicked();
}
