// Copyright (c) 2020 The NoteBlockchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeman.h"
#include "fs.h"
#include "streams.h"
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

bool CMasternodeMan::Save()
{
    fs::path pathMNCache = GetDataDir() / "mncache.dat";

    try {
        CAutoFile file(fsbridge::fopen(pathMNCache, "wb"), SER_DISK, CLIENT_VERSION);
        if (file.IsNull()) {
            LogPrintf("CMasternodeMan::Save: Failed to open file %s\n", pathMNCache.string());
            return false;
        }

        LOCK(cs);
        file << *this;
        LogPrintf("CMasternodeMan::Save: Saved %d masternodes to %s\n", mapMasternodes.size(), pathMNCache.string());
        return true;
    }
    catch (const std::exception& e) {
        LogPrintf("CMasternodeMan::Save: Exception - %s\n", e.what());
        return false;
    }
}

bool CMasternodeMan::Load()
{
    fs::path pathMNCache = GetDataDir() / "mncache.dat";

    if (!fs::exists(pathMNCache)) {
        LogPrintf("CMasternodeMan::Load: Cache file %s does not exist\n", pathMNCache.string());
        return false;
    }

    try {
        CAutoFile file(fsbridge::fopen(pathMNCache, "rb"), SER_DISK, CLIENT_VERSION);
        if (file.IsNull()) {
            LogPrintf("CMasternodeMan::Load: Failed to open file %s\n", pathMNCache.string());
            return false;
        }

        LOCK(cs);
        file >> *this;
        LogPrintf("CMasternodeMan::Load: Loaded %d masternodes from %s\n", mapMasternodes.size(), pathMNCache.string());

        // Update last seen times for loaded masternodes
        for (auto& pair : mapMasternodes) {
            pair.second.UpdateLastSeen();
        }

        return true;
    }
    catch (const std::exception& e) {
        LogPrintf("CMasternodeMan::Load: Exception - %s\n", e.what());
        return false;
    }
}
