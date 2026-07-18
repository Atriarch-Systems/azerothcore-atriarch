#include "mod-ollama-chat_rumors.h"
#include "mod-ollama-chat_config.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <vector>

namespace
{
    struct Sighting
    {
        std::string playerName;
        std::string zoneName;
        time_t at;
    };

    std::mutex g_rumorMutex;
    std::unordered_map<uint64_t, std::deque<Sighting>> g_sightings;
    bool g_rumorsDirty = false;
    time_t g_lastRumorSave = 0;

    bool RumorsEnabled()
    {
        static bool cached = sConfigMgr->GetOption<bool>("OllamaChat.EnableRumors", true);
        return cached;
    }

    size_t MaxSightings()
    {
        static size_t cached = sConfigMgr->GetOption<uint32_t>("OllamaChat.RumorMaxSightings", 5);
        return cached ? cached : 5;
    }

    time_t TtlSeconds()
    {
        static time_t cached = static_cast<time_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.RumorTTLDays", 14)) * 86400;
        return cached ? cached : (14 * 86400);
    }

    std::string ZoneNameOf(Player* p)
    {
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId()))
            if (zone->area_name[0])
                return zone->area_name[0];
        return "parts unknown";
    }

    std::string AgoPhrase(time_t now, time_t at)
    {
        long mins = static_cast<long>((now - at) / 60);
        if (mins <= 1)
            return "moments ago";
        if (mins < 120)
            return "about " + std::to_string(mins) + " minutes ago";
        long hours = mins / 60;
        if (hours < 48)
            return "about " + std::to_string(hours) + " hours ago";
        return "about " + std::to_string(hours / 24) + " days ago";
    }
}

void RumorRecordSighting(Player* bot, Player* realPlayer)
{
    if (!RumorsEnabled() || !bot || !realPlayer)
        return;

    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(g_rumorMutex);
    auto& dq = g_sightings[bot->GetGUID().GetRawValue()];

    // Refresh an existing sighting of the same player instead of duplicating
    for (auto& s : dq)
    {
        if (s.playerName == realPlayer->GetName())
        {
            s.zoneName = ZoneNameOf(realPlayer);
            s.at = now;
            g_rumorsDirty = true;
            return;
        }
    }

    dq.push_back({ realPlayer->GetName(), ZoneNameOf(realPlayer), now });
    while (dq.size() > MaxSightings())
        dq.pop_front();
    g_rumorsDirty = true;
}

std::string GetRumorPromptText(Player* bot)
{
    if (!RumorsEnabled() || !bot)
        return "";

    time_t now = time(nullptr);
    std::ostringstream oss;
    std::lock_guard<std::mutex> lock(g_rumorMutex);

    auto it = g_sightings.find(bot->GetGUID().GetRawValue());
    if (it == g_sightings.end())
        return "";

    auto& dq = it->second;
    while (!dq.empty() && (now - dq.front().at) > TtlSeconds())
        dq.pop_front();
    if (dq.empty())
        return "";

    for (auto const& s : dq)
        oss << " You personally saw " << s.playerName << " in " << s.zoneName
            << " " << AgoPhrase(now, s.at) << ".";
    oss << " If someone asks who you have seen, answer honestly from this (in character).";
    return oss.str();
}

void LoadRumorsFromDB()
{
    if (!RumorsEnabled())
        return;

    time_t cutoff = time(nullptr) - TtlSeconds();
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, player_name, zone, epoch FROM mod_ollama_chat_rumors WHERE epoch >= {} ORDER BY epoch ASC", cutoff);

    uint32 rows = 0;
    if (result)
    {
        std::lock_guard<std::mutex> lock(g_rumorMutex);
        do
        {
            Field* f = result->Fetch();
            uint64_t guid = f[0].Get<uint64>();
            auto& dq = g_sightings[guid];
            dq.push_back({ f[1].Get<std::string>(), f[2].Get<std::string>(), static_cast<time_t>(f[3].Get<uint64>()) });
            while (dq.size() > MaxSightings())
                dq.pop_front();
            ++rows;
        } while (result->NextRow());
    }

    // Prune anything expired on disk
    CharacterDatabase.Execute("DELETE FROM mod_ollama_chat_rumors WHERE epoch < {}", cutoff);
    LOG_INFO("server.loading", "[Ollama Chat] Loaded {} rumor sightings from DB.", rows);
}

void SaveRumorsToDB()
{
    if (!RumorsEnabled())
        return;

    time_t now = time(nullptr);
    time_t cutoff = now - TtlSeconds();

    std::unordered_map<uint64_t, std::deque<Sighting>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_rumorMutex);
        if (!g_rumorsDirty)
            return;
        g_rumorsDirty = false;
        snapshot = g_sightings;
    }

    for (auto const& [guid, dq] : snapshot)
    {
        for (auto const& s : dq)
        {
            if (s.at < cutoff)
                continue;
            std::string name = s.playerName;
            std::string zone = s.zoneName;
            CharacterDatabase.EscapeString(name);
            CharacterDatabase.EscapeString(zone);
            CharacterDatabase.Execute(
                "REPLACE INTO mod_ollama_chat_rumors (guid, player_name, zone, epoch) VALUES ({}, '{}', '{}', {})",
                guid, name, zone, static_cast<uint64_t>(s.at));
        }
    }
    CharacterDatabase.Execute("DELETE FROM mod_ollama_chat_rumors WHERE epoch < {}", cutoff);
}

void MaybeSaveRumorsToDB()
{
    // Reuse the conversation-history cadence (minutes); 0 disables periodic saves.
    if (g_ConversationHistorySaveInterval == 0)
        return;
    time_t now = time(nullptr);
    if (difftime(now, g_lastRumorSave) < g_ConversationHistorySaveInterval * 60)
        return;
    g_lastRumorSave = now;
    SaveRumorsToDB();
}

// ---------------------------------------------------------------------------
// Seeker journey integration
// ---------------------------------------------------------------------------
namespace
{
    std::string SeekerNameLower()
    {
        static std::string cached = []() {
            std::string s = sConfigMgr->GetOption<std::string>("OllamaBotControl.SeekerName", "");
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        }();
        return cached;
    }

    bool IsSeekerByName(Player* p)
    {
        if (!p) return false;
        std::string const& seeker = SeekerNameLower();
        if (seeker.empty()) return false;
        std::string n = p->GetName();
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return n == seeker;
    }

    bool SeekerTablesExist()
    {
        static bool cached = []() {
            QueryResult r = CharacterDatabase.Query(
                "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_ollama_seeker_chronicle' LIMIT 1");
            return bool(r);
        }();
        return cached;
    }
}

std::string GetSeekerJourneyPromptText(Player* bot)
{
    if (!IsSeekerByName(bot) || !SeekerTablesExist())
        return "";

    static std::mutex cacheMutex;
    static std::string cachedTail;
    static time_t lastFetch = 0;

    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (now - lastFetch >= 120)
    {
        lastFetch = now;
        cachedTail.clear();
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT chapter_text FROM mod_ollama_seeker_chronicle WHERE guid = {} ORDER BY id DESC LIMIT 2",
            bot->GetGUID().GetRawValue()))
        {
            std::vector<std::string> chapters;
            do { chapters.push_back((*r)[0].Get<std::string>()); } while (r->NextRow());
            for (auto it = chapters.rbegin(); it != chapters.rend(); ++it)
                cachedTail += *it + " ";
        }
    }
    if (cachedTail.empty())
        return "";
    return " YOUR JOURNEY SO FAR (your own memories - draw on them when asked about your travels): " + cachedTail;
}

void RecordSeekerConversation(Player* possibleSeekerSender, Player* speakingBot, std::string const& text)
{
    if (!IsSeekerByName(possibleSeekerSender) || !speakingBot || !SeekerTablesExist())
        return;

    std::string target = speakingBot->GetName();
    std::string say = text.substr(0, 120);
    CharacterDatabase.EscapeString(target);
    CharacterDatabase.EscapeString(say);
    CharacterDatabase.Execute(
        "INSERT INTO mod_ollama_seeker_journal (guid, event_type, directive, target, say) VALUES ({}, 'conversation', 'reply_heard', '{}', '{}')",
        possibleSeekerSender->GetGUID().GetRawValue(), target, say);
}
