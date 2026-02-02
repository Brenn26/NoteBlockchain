// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"
#include "hash.h"
#include "util.h"
#include "utiltime.h"

CMasternode::CMasternode() :
    outpoint(),
    addr(),
    pubKeyMasternode(),
    sigTime(0),
    nActiveState(MASTERNODE_ENABLED),
    nTimeLastSeen(0),
    nProtocolVersion(0),
    nTimeCreated(GetAdjustedTime())
{
}

CMasternode::CMasternode(const COutPoint& outpointIn, const CService& addrIn, const CPubKey& pubKeyIn) :
    outpoint(outpointIn),
    addr(addrIn),
    pubKeyMasternode(pubKeyIn),
    sigTime(GetAdjustedTime()),
    nActiveState(MASTERNODE_ENABLED),
    nTimeLastSeen(GetAdjustedTime()),
    nProtocolVersion(PROTOCOL_VERSION),
    nTimeCreated(GetAdjustedTime())
{
}

bool CMasternode::IsExpired() const
{
    // Consider masternode expired if not seen for 2 hours
    return (GetAdjustedTime() - nTimeLastSeen) > (2 * 60 * 60);
}

void CMasternode::UpdateLastSeen(int64_t nTime)
{
    LOCK(cs);
    nTimeLastSeen = (nTime == 0) ? GetAdjustedTime() : nTime;
}

uint256 CMasternode::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << outpoint;
    ss << sigTime;
    return ss.GetHash();
}

std::string CMasternode::GetStatus() const
{
    switch (nActiveState) {
        case MASTERNODE_ENABLED:
            return "ENABLED";
        case MASTERNODE_EXPIRED:
            return "EXPIRED";
        case MASTERNODE_DISABLED:
            return "DISABLED";
        default:
            return "UNKNOWN";
    }
}
