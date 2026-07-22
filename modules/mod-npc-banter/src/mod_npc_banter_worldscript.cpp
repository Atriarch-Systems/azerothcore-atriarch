#include "mod_npc_banter_worldscript.h"
#include "mod_npc_banter_config.h"
#include "mod_npc_banter_prompt.h"
#include "mod_npc_banter_registry.h"
#include "mod-ollama-chat_api.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <thread>

namespace
{
    // Our OWN smaller sub-cap, enforced BEFORE ever calling
    // g_queryManager.submitQuery() - this never raises the shared
    // OllamaChat.MaxConcurrentQueries ceiling, it only ever claims up to
    // NpcBanter.MaxConcurrentQueries of those slots for itself.
    std::atomic<uint32_t> g_activeQueries{ 0 };

    // An async completion, marshaled back onto the world thread. Never carries
    // a raw Creature*/Player* - only the guids (+ mapId/instanceId, since a
    // bare guid has no map context) needed to re-resolve them fresh.
    struct PendingBanterResult
    {
        ObjectGuid  creatureGuid;
        uint32_t    mapId = 0;
        uint32_t    instanceId = 0;
        ObjectGuid  playerGuid;
        std::string text; // already validated/filtered, or a canned fallback
    };

    std::mutex g_pendingMutex;
    std::deque<PendingBanterResult> g_pendingResults;

    void PushPendingResult(PendingBanterResult result)
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingResults.push_back(std::move(result));
    }

    void SpeakToPlayer(Creature* creature, Player* player, std::string const& text)
    {
        if (text.empty())
            return; // silence is an acceptable outcome; a stall or crash is not

        if (g_NpcBanterWhisperOnly)
            creature->Whisper(text, LANG_UNIVERSAL, player, false);
        else
            creature->Say(text, LANG_UNIVERSAL);
    }

    // Runs on the world thread only. Re-resolves both sides fresh from guids -
    // a guard can die/despawn/have its grid unload, and a player can log out
    // or zone away, during the ~1s+ LLM round trip.
    void DeliverPendingResult(PendingBanterResult const& pr)
    {
        // Clear the in-flight flag regardless of delivery outcome, so a
        // despawned/removed NPC never leaves a phantom "pending" registration.
        NpcBanterRuntimeState state;
        if (sNpcBanterRegistry.TryGet(pr.creatureGuid, state))
        {
            state.pending = false;
            sNpcBanterRegistry.Update(pr.creatureGuid, state);
        }

        Map* map = sMapMgr->FindMap(pr.mapId, pr.instanceId);
        Creature* creature = map ? map->GetCreature(pr.creatureGuid) : nullptr;
        if (!creature || !creature->IsInWorld())
            return; // despawned/unloaded mid-flight - drop it, never retry

        Player* player = ObjectAccessor::FindPlayer(pr.playerGuid);
        if (!player || !player->IsInWorld())
            return; // player logged out/zoned mid-flight

        SpeakToPlayer(creature, player, pr.text);
    }

    void DrainPendingBanterResults()
    {
        std::deque<PendingBanterResult> local;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            local.swap(g_pendingResults);
        }
        for (auto const& pr : local)
            DeliverPendingResult(pr);
    }

    time_t RollNextCooldown(time_t now)
    {
        uint32_t lo = g_NpcBanterMinCooldown;
        uint32_t hi = std::max(g_NpcBanterMaxCooldown, lo);
        return now + time_t(urand(lo, hi));
    }

    // Kicks off the LLM call on a background worker thread that touches
    // nothing but strings, QueryManager's own future, and the pending-results
    // queue above - never the creature/player objects themselves. This is the
    // one and only path that ever calls g_queryManager.submitQuery(): it is
    // gated by the g_activeQueries sub-cap check in OnUpdate before this is
    // ever invoked, and it never bypasses that shared QueryManager semaphore
    // with a raw detached call straight into the HTTP layer.
    void DispatchLlmBanter(ObjectGuid creatureGuid, uint32_t mapId, uint32_t instanceId,
                            ObjectGuid playerGuid, std::string prompt,
                            std::string archetypeKey, std::string cannedFallback)
    {
        ++g_activeQueries;

        std::thread([creatureGuid, mapId, instanceId, playerGuid,
                     prompt = std::move(prompt), archetypeKey = std::move(archetypeKey),
                     cannedFallback = std::move(cannedFallback)]()
        {
            std::string text;
            try
            {
                std::future<std::string> future = g_queryManager.submitQuery(prompt);
                text = future.get();
            }
            catch (std::exception const&)
            {
                text.clear();
            }

            text = SanitizeNpcBanterResponse(text);

            if (text.empty() || NpcBanterMatchesBannedTopic(text))
                text = cannedFallback; // never regenerate - that's a second GPU call
            else
                RememberNpcBanterLine(archetypeKey, text);

            PushPendingResult({ creatureGuid, mapId, instanceId, playerGuid, text });
            --g_activeQueries;
        }).detach();
    }
}

NpcBanterWorldScript::NpcBanterWorldScript() : WorldScript("NpcBanterWorldScript") { }

void NpcBanterWorldScript::OnUpdate(uint32 /*diff*/)
{
    // Always drain first, independent of the enable flag below, so a request
    // already in flight when the module is disabled mid-session still lands
    // cleanly instead of leaking a permanently-pending registry entry.
    DrainPendingBanterResults();

    if (!g_NpcBanterEnable)
        return;

    static time_t nextTick = 0;
    time_t now = time(nullptr);
    if (now < nextTick)
        return;
    nextTick = now + time_t(g_NpcBanterTickSeconds);

    for (ObjectGuid const& guid : sNpcBanterRegistry.SnapshotGuids())
    {
        NpcBanterRuntimeState state;
        if (!sNpcBanterRegistry.TryGet(guid, state))
            continue; // despawned since the snapshot was taken

        // Defensive staleness recovery: a request that never completed (dead
        // Ollama endpoint, crashed worker) must not permanently wedge this
        // one NPC out of ever bantering again.
        if (state.pending && (now - state.pendingSince) > 90)
            state.pending = false;

        if (state.pending)
            continue;

        if (now < state.nextBanterAt)
            continue;

        Map* map = sMapMgr->FindMap(state.mapId, state.instanceId);
        Creature* creature = map ? map->GetCreature(guid) : nullptr;
        if (!creature || !creature->IsInWorld())
            continue;

        Player* nearest = creature->SelectNearestPlayer(g_NpcBanterBanterRange);
        if (!nearest)
            continue;

        // SelectNearestPlayer matches any Player, bot or real - playerbots must
        // be filtered out explicitly.
        if (PlayerbotsMgr::instance().GetPlayerbotAI(nearest))
            continue;

        // Short-window immediate-refire suppressor ONLY (a few seconds) - NOT
        // a long-term "don't greet the same visitor twice" gate. The cooldown
        // above is the only long-term re-trigger gate; see docs/npc-banter.md
        // section 3 for why a same-visitor AND-condition would be wrong here.
        if (state.lastGreetedGuid == nearest->GetGUID() && (now - state.lastGreetedAt) < 5)
            continue;

        state.lastGreetedGuid = nearest->GetGUID();
        state.lastGreetedAt = now;
        state.nextBanterAt = RollNextCooldown(now);

        std::string cannedFallback = PickNpcBanterCannedLine(state.archetypeKey);

        // Tier 1: replay a recently-generated line verbatim instead of calling
        // the LLM again, at NpcBanter.CacheReuseChance%.
        if (roll_chance_f(float(g_NpcBanterCacheReuseChance)))
        {
            std::string cached = PickNpcBanterCachedLine(state.archetypeKey);
            if (!cached.empty())
            {
                sNpcBanterRegistry.Update(guid, state);
                SpeakToPlayer(creature, nearest, cached);
                continue;
            }
        }

        // Tier 2 (immediate): our own sub-cap is full - never queue, never
        // wait, just say the canned line instead.
        if (g_activeQueries.load() >= g_NpcBanterMaxConcurrentQueries)
        {
            sNpcBanterRegistry.Update(guid, state);
            SpeakToPlayer(creature, nearest, cannedFallback);
            continue;
        }

        // Real generation: dispatch async, mark pending, and let
        // DrainPendingBanterResults() speak whatever comes back (or the
        // canned fallback baked into the dispatch, on failure).
        state.pending = true;
        state.pendingSince = now;
        sNpcBanterRegistry.Update(guid, state);

        std::string prompt = BuildNpcBanterPrompt(
            state.archetypeKey, state.backstory, creature->GetName(), nearest->GetName());

        DispatchLlmBanter(guid, state.mapId, state.instanceId, nearest->GetGUID(),
            std::move(prompt), state.archetypeKey, std::move(cannedFallback));
    }
}
