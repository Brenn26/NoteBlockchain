// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_MASTERNODEMAN_H
#define BITCOIN_MASTERNODE_MASTERNODEMAN_H

#include "masternode.h"
#include "serialize.h"
#include "sync.h"
#include <map>
#include <vector>

/**
 * Masternode Manager - tracks all masternodes on the network
 */
class CMasternodeMan
{
private:
    mutable CCriticalSection cs;
    std::map<COutPoint, CMasternode> mapMasternodes;

public:
    CMasternodeMan() {}

    // Add a masternode
    bool Add(CMasternode& mn);

    // Remove a masternode
    bool Remove(const COutPoint& outpoint);

    // Find a masternode by outpoint
    CMasternode* Find(const COutPoint& outpoint);

    // Check if masternode exists
    bool Has(const COutPoint& outpoint);

    // Check if IP address already has a masternode
    bool HasIP(const CService& addr);

    // Find masternode by IP address
    CMasternode* FindByIP(const CService& addr);

    // Get count of masternodes
    int size() const { LOCK(cs); return mapMasternodes.size(); }

    // Get count of enabled masternodes
    int CountEnabled() const;

    // Get all enabled masternodes
    std::vector<CMasternode> GetFullMasternodeVector();

    // Get next masternode for payment (used in traditional masternode payment)
    CMasternode* GetNextMasternodeInQueueForPayment(int nBlockHeight);

    // Clear all masternodes
    void Clear();

    // Check and remove expired masternodes
    void CheckAndRemove();

    // Persistence
    bool Save();
    bool Load();

    std::string ToString() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        READWRITE(mapMasternodes);
    }
};

// Global masternode manager
extern CMasternodeMan mnodeman;

#endif // BITCOIN_MASTERNODE_MASTERNODEMAN_H
