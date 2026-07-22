#include "mod_npc_banter_registry.h"
#include "mod_npc_banter_config.h"
#include "Creature.h"
#include "Log.h"
#include "Random.h"
#include <ctime>

NpcBanterRegistry& NpcBanterRegistry::instance()
{
    static NpcBanterRegistry inst;
    return inst;
}

void NpcBanterRegistry::Add(ObjectGuid const& guid, NpcBanterRuntimeState state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states[guid] = std::move(state);
}

void NpcBanterRegistry::Remove(ObjectGuid const& guid)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states.erase(guid);
}

std::vector<ObjectGuid> NpcBanterRegistry::SnapshotGuids()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ObjectGuid> guids;
    guids.reserve(m_states.size());
    for (auto const& entry : m_states)
        guids.push_back(entry.first);
    return guids;
}

bool NpcBanterRegistry::TryGet(ObjectGuid const& guid, NpcBanterRuntimeState& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_states.find(guid);
    if (it == m_states.end())
        return false;
    out = it->second;
    return true;
}

void NpcBanterRegistry::Update(ObjectGuid const& guid, NpcBanterRuntimeState const& state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_states.find(guid);
    if (it != m_states.end())
        it->second = state;
    // Not found: the creature despawned since the caller last read it. Drop
    // the update silently rather than resurrecting a stale registration.
}

NpcBanterCreatureScript::NpcBanterCreatureScript() : AllCreatureScript("NpcBanterCreatureScript") { }

void NpcBanterCreatureScript::OnCreatureAddWorld(Creature* creature)
{
    if (!creature)
        return;

    NpcBanterConfigRow const* row = FindNpcBanterConfigRow(creature->GetSpawnId());
    if (!row || !row->enabled)
        return;

    // Integrity check: a GM deleting/re-adding a spawn (or the spawn-id
    // allocator otherwise reusing a guid) must not silently attach a stale
    // archetype/backstory to an unrelated creature.
    if (row->entry != creature->GetEntry())
    {
        LOG_WARN("module.npcbanter",
            "[NpcBanter] Spawn guid {} entry mismatch (config expects entry {}, live creature is "
            "entry {}) - skipping banter registration. Update mod_npc_banter_config if this spawn "
            "was intentionally replaced.",
            row->guid, row->entry, creature->GetEntry());
        return;
    }

    NpcBanterRuntimeState state;
    state.spawnId      = row->guid;
    state.entry        = row->entry;
    state.mapId        = creature->GetMapId();
    state.instanceId   = creature->GetInstanceId();
    state.archetypeKey = row->archetypeKey;
    state.backstory    = row->backstory;

    // Stagger the first eligible banter so a world restart doesn't line every
    // registered NPC up on the same tick.
    state.nextBanterAt = time(nullptr) +
        time_t(irand(int32(g_NpcBanterMinCooldown), int32(g_NpcBanterMaxCooldown)));

    sNpcBanterRegistry.Add(creature->GetGUID(), std::move(state));
}

void NpcBanterCreatureScript::OnCreatureRemoveWorld(Creature* creature)
{
    if (!creature)
        return;

    sNpcBanterRegistry.Remove(creature->GetGUID());
}
