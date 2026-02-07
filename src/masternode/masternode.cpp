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

bool CMasternode::Sign(const CKey& keyMasternode)
{
    sigTime = GetAdjustedTime();

    // Create message to sign: outpoint + addr + sigTime
    std::string strMessage = outpoint.ToString() + addr.ToString() + std::to_string(sigTime);

    // Hash the message with Bitcoin's message magic
    std::string strMessageMagic = "NoteBlockchain Signed Message:\n";
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    // Sign the message using compact signature
    if (!keyMasternode.SignCompact(ss.GetHash(), vchSig)) {
        LogPrintf("CMasternode::Sign: Failed to sign masternode announcement\n");
        return false;
    }

    return true;
}

bool CMasternode::VerifySignature() const
{
    // Create the same message that was signed
    std::string strMessage = outpoint.ToString() + addr.ToString() + std::to_string(sigTime);

    // Hash the message with the same magic
    std::string strMessageMagic = "NoteBlockchain Signed Message:\n";
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    // Recover the public key from the signature
    CPubKey pubkeyFromSig;
    if (!pubkeyFromSig.RecoverCompact(ss.GetHash(), vchSig)) {
        LogPrintf("CMasternode::VerifySignature: Failed to recover pubkey from signature for %s\n", outpoint.ToString());
        return false;
    }

    // Verify that the recovered pubkey matches the claimed masternode pubkey
    if (pubkeyFromSig.GetID() != pubKeyMasternode.GetID()) {
        LogPrintf("CMasternode::VerifySignature: Recovered pubkey doesn't match claimed pubkey for %s\n", outpoint.ToString());
        return false;
    }

    return true;
}
