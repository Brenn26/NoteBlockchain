// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODELIST_H
#define BITCOIN_QT_MASTERNODELIST_H

#include <QWidget>
#include <QTimer>

class ClientModel;
class WalletModel;

namespace Ui {
    class MasternodeList;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Masternode Manager page widget */
class MasternodeList : public QWidget
{
    Q_OBJECT

public:
    explicit MasternodeList(QWidget *parent = 0);
    ~MasternodeList();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);

private:
    QTimer *timer;
    Ui::MasternodeList *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;

public Q_SLOTS:
    void updateMyNodeList();
    void updateNetworkList();

Q_SIGNALS:

private Q_SLOTS:
    void on_startButton_clicked();
    void on_stopButton_clicked();
};

#endif // BITCOIN_QT_MASTERNODELIST_H
