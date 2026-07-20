#ifndef PLAYERBOTS_MANAGEDBOTREGISTRY_H
#define PLAYERBOTS_MANAGEDBOTREGISTRY_H

#include "Define.h"
#include <mutex>
#include <unordered_set>

// Registry of bots/groups currently driven by an external orchestrator
// (mod-ollama-bot-buddy's guild dungeon runs, raid lab, etc.).
//
// RandomPlayerbotMgr::ProcessBot would otherwise disband managed groups,
// re-randomize gear/talents mid-run, teleport the leader to a grind zone, and
// revive-teleport dead members away from their corpses. It also logs bots out
// when their in-world window expires (default 600s), which is far shorter than
// a dungeon run.
//
// This lives in mod-playerbots (rather than being a call into the other
// module) so the core loop has no forward dependency: the orchestrator simply
// registers ids here. Reads are lock-free against a snapshot pointer because
// ProcessBot/UpdateAIGroupMaster run on the hot path for hundreds of bots;
// writes happen only on run start/stop.
class ManagedBotRegistry
{
public:
    static ManagedBotRegistry& instance()
    {
        static ManagedBotRegistry inst;
        return inst;
    }

    void AddBot(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto next = std::make_shared<std::unordered_set<uint32>>(*_bots);
        next->insert(guidLow);
        std::atomic_store(&_bots, next);
    }

    void RemoveBot(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto next = std::make_shared<std::unordered_set<uint32>>(*_bots);
        next->erase(guidLow);
        std::atomic_store(&_bots, next);
    }

    void AddGroup(uint32 groupId)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto next = std::make_shared<std::unordered_set<uint32>>(*_groups);
        next->insert(groupId);
        std::atomic_store(&_groups, next);
    }

    void RemoveGroup(uint32 groupId)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto next = std::make_shared<std::unordered_set<uint32>>(*_groups);
        next->erase(groupId);
        std::atomic_store(&_groups, next);
    }

    bool IsManagedBot(uint32 guidLow) const
    {
        auto snap = std::atomic_load(&_bots);
        return snap->find(guidLow) != snap->end();
    }

    bool IsManagedGroup(uint32 groupId) const
    {
        auto snap = std::atomic_load(&_groups);
        return snap->find(groupId) != snap->end();
    }

    bool Empty() const
    {
        return std::atomic_load(&_bots)->empty() && std::atomic_load(&_groups)->empty();
    }

private:
    ManagedBotRegistry()
        : _bots(std::make_shared<std::unordered_set<uint32>>()),
          _groups(std::make_shared<std::unordered_set<uint32>>())
    {
    }

    mutable std::mutex _mutex;
    std::shared_ptr<std::unordered_set<uint32>> _bots;
    std::shared_ptr<std::unordered_set<uint32>> _groups;
};

#define sManagedBotRegistry ManagedBotRegistry::instance()

#endif
