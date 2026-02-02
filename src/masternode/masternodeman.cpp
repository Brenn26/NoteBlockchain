// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeman.h"
#include "util.h"
#include "utiltime.h"

CMasternodeMan mnodeman;

bool CMasternodeMan::Add(CMasternode& mn)
{
    LOCK(cs);

    if (Has(mn.outpoint))
        return false;

    mapMasternodes[mn.outpoint] = mn;
    return true;
}

bool CMasternodeMan::Remove(const COutPoint& outpoint)
{
    LOCK(cs);

    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end())
        return false;

    mapMasternodes.erase(it);
    return true;
}

CMasternode* CMasternodeMan::Find(const COutPoint& outpoint)
{
    LOCK(cs);

    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end())
        return nullptr;

    return &(it->second);
}

bool CMasternodeMan::Has(const COutPoint& outpoint)
{
    LOCK(cs);
    return mapMasternodes.find(outpoint) != mapMasternodes.end();
}

bool CMasternodeMan::HasIP(const CService& addr)
{
    LOCK(cs);
    for (const auto& pair : mapMasternodes) {
        if (pair.second.addr == addr && pair.second.IsEnabled()) {
            return true;
        }
    }
    return false;
}

CMasternode* CMasternodeMan::FindByIP(const CService& addr)
{
    LOCK(cs);
    for (auto& pair : mapMasternodes) {
        if (pair.second.addr == addr && pair.second.IsEnabled()) {
            return &pair.second;
        }
    }
    return nullptr;
}

int CMasternodeMan::CountEnabled() const
{
    LOCK(cs);
    int count = 0;
    for (const auto& pair : mapMasternodes) {
        if (pair.second.IsEnabled() && !pair.second.IsExpired())
            count++;
    }
    return count;
}

std::vector<CMasternode> CMasternodeMan::GetFullMasternodeVector()
{
    LOCK(cs);
    std::vector<CMasternode> result;
    result.reserve(mapMasternodes.size());

    for (const auto& pair : mapMasternodes) {
        if (pair.second.IsEnabled() && !pair.second.IsExpired())
            result.push_back(pair.second);
    }

    return result;
}

CMasternode* CMasternodeMan::GetNextMasternodeInQueueForPayment(int nBlockHeight)
{
    LOCK(cs);

    // Simple round-robin based on block height
    // In production, you might want a more sophisticated selection
    std::vector<CMasternode*> enabledNodes;

    for (auto& pair : mapMasternodes) {
        if (pair.second.IsEnabled() && !pair.second.IsExpired())
            enabledNodes.push_back(&pair.second);
    }

    if (enabledNodes.empty())
        return nullptr;

    // Select based on block height modulo number of masternodes
    size_t index = nBlockHeight % enabledNodes.size();
    return enabledNodes[index];
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
}

void CMasternodeMan::CheckAndRemove()
{
    LOCK(cs);

    std::vector<COutPoint> toRemove;

    for (auto& pair : mapMasternodes) {
        if (pair.second.IsExpired()) {
            toRemove.push_back(pair.first);
        }
    }

    for (const auto& outpoint : toRemove) {
        mapMasternodes.erase(outpoint);
    }

    if (!toRemove.empty()) {
        LogPrintf("CMasternodeMan::CheckAndRemove: Removed %d expired masternodes\n", toRemove.size());
    }
}

std::string CMasternodeMan::ToString() const
{
    LOCK(cs);
    return strprintf("Masternodes: %d (enabled: %d)", mapMasternodes.size(), CountEnabled());
}
