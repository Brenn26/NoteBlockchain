// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_MASTERNODE_H
#define BITCOIN_MASTERNODE_MASTERNODE_H

#include "key.h"
#include "net.h"
#include "primitives/transaction.h"
#include "sync.h"
#include "uint256.h"

/**
 * A Masternode that can stake blocks in the PoS system
 */
class CMasternode
{
private:
    mutable CCriticalSection cs;

public:
    enum State {
        MASTERNODE_ENABLED = 1,
        MASTERNODE_EXPIRED = 2,
        MASTERNODE_DISABLED = 3
    };

    COutPoint outpoint;              // Collateral outpoint (txid + n)
    CService addr;                   // Masternode IP:port
    CPubKey pubKeyMasternode;        // Masternode public key for payments
    int64_t sigTime;                 // Signature timestamp
    int nActiveState;                // Current state
    int64_t nTimeLastSeen;           // Last time masternode was seen
    int nProtocolVersion;            // Protocol version
    int64_t nTimeCreated;            // When masternode was registered

    CMasternode();
    CMasternode(const COutPoint& outpointIn, const CService& addrIn, const CPubKey& pubKeyIn);

    SERIALIZE_METHODS(CMasternode, obj)
    {
        LOCK(obj.cs);
        READWRITE(obj.outpoint);
        READWRITE(obj.addr);
        READWRITE(obj.pubKeyMasternode);
        READWRITE(obj.sigTime);
        READWRITE(obj.nActiveState);
        READWRITE(obj.nTimeLastSeen);
        READWRITE(obj.nProtocolVersion);
        READWRITE(obj.nTimeCreated);
    }

    bool IsEnabled() const { return nActiveState == MASTERNODE_ENABLED; }
    bool IsExpired() const;

    void UpdateLastSeen(int64_t nTime = 0);

    uint256 GetHash() const;
    std::string GetStatus() const;
};

#endif // BITCOIN_MASTERNODE_MASTERNODE_H
