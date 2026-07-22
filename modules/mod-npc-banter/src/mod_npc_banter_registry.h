#ifndef MOD_NPC_BANTER_REGISTRY_H
#define MOD_NPC_BANTER_REGISTRY_H

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "ObjectGuid.h"
#include "ScriptMgr.h" // AllCreatureScript

class Creature;

// Bookkeeping kept for every currently-in-world NPC that is opted in to
// banter. Populated purely from OnCreatureAddWorld/OnCreatureRemoveWorld and
// never holds a raw Creature* - WorldScript::OnUpdate re-resolves the live
// Creature* fresh every tick via sMapMgr->FindMap(mapId, instanceId)->GetCreature(guid),
// which is only ever safe synchronously within that same OnUpdate call (see
// the grounding notes carried in the delivering agent's final report).
struct NpcBanterRuntimeState
{
    uint32_t    spawnId = 0;
    uint32_t    entry = 0;
    uint32_t    mapId = 0;
    uint32_t    instanceId = 0;
    std::string archetypeKey;
    std::string backstory;

    time_t nextBanterAt = 0;   // long-term cooldown gate - the ONLY long-term re-trigger gate
    bool   pending = false;    // an async LLM query is currently in flight for this NPC
    time_t pendingSince = 0;   // staleness recovery if that async call never lands

    ObjectGuid lastGreetedGuid; // short-window immediate-refire suppressor ONLY, not a dedup key
    time_t     lastGreetedAt = 0;
};

// Thread-safe registry, keyed by the live Creature's ObjectGuid. Writes can
// legitimately arrive from a map worker thread (a creature loading on a
// different map while MapUpdate.Threads > 1); reads happen from the world
// thread inside NpcBanterWorldScript::OnUpdate. The mutex protects the map's
// internal structure across those callers, not any particular pointer.
class NpcBanterRegistry
{
public:
    static NpcBanterRegistry& instance();

    void Add(ObjectGuid const& guid, NpcBanterRuntimeState state);
    void Remove(ObjectGuid const& guid);

    // Snapshot of every registered guid, to iterate outside the lock.
    std::vector<ObjectGuid> SnapshotGuids();

    // Copies the current state for guid into out; false if no longer registered.
    bool TryGet(ObjectGuid const& guid, NpcBanterRuntimeState& out);

    // Replaces the stored state for guid; silently dropped if guid is no
    // longer registered (the creature despawned since the caller last read it).
    void Update(ObjectGuid const& guid, NpcBanterRuntimeState const& state);

private:
    std::mutex m_mutex;
    std::unordered_map<ObjectGuid, NpcBanterRuntimeState> m_states;
};

#define sNpcBanterRegistry NpcBanterRegistry::instance()

// Additive-only: never sets ScriptName/AIName and never calls GetAI() on the
// target creature. CreatureAISelector's normal AI vote (GuardAI, PassiveAI,
// whatever npc_innkeeper.cpp/npc_taxi.cpp already assign) is untouched.
class NpcBanterCreatureScript : public AllCreatureScript
{
public:
    NpcBanterCreatureScript();
    void OnCreatureAddWorld(Creature* creature) override;
    void OnCreatureRemoveWorld(Creature* creature) override;
};

#endif // MOD_NPC_BANTER_REGISTRY_H
