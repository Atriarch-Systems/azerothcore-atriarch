#include "mod-ollama-bot-buddy_loop.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_handler.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Log.h"
#include <thread>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include "httplib.h"
#include <ctime>
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "PathGenerator.h"
#include <atomic>
#include <unordered_map>
#include <iomanip>
#include "GameObjectData.h"
#include "GameObject.h"
#include <deque>
#include <mutex>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include <algorithm>
#include "Chat.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <string>
#include "ItemTemplate.h"
#include "CreatureData.h"
#include "CharacterCache.h"
#include "RandomPlayerbotMgr.h"
#include "SocialMgr.h"
#include "DatabaseEnv.h"
#include <unordered_set>


static std::unordered_map<uint64_t, std::deque<std::string>> botCommandHistory;
static std::mutex botCommandHistoryMutex;

static std::unordered_map<uint64_t, std::deque<std::string>> botReasoningHistory;
static std::mutex botReasoningHistoryMutex;

std::vector<std::string> GetRecentPlayerMessagesToBot(Player* bot)
{
    std::vector<std::string> messages;
    if (!bot) return messages;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto it = botPlayerMessages.find(bot->GetGUID().GetRawValue());
    if (it != botPlayerMessages.end())
    {
        while (!it->second.empty())
        {
            auto& [senderName, msg] = it->second.front();
            messages.emplace_back("From " + senderName + ": " + msg);
            it->second.pop_front();
        }
    }

    return messages;
}

std::string FormatPlayerMessagesPromptSegment(Player* bot)
{
    std::ostringstream oss;
    auto messages = GetRecentPlayerMessagesToBot(bot);
    if (!messages.empty())
    {
        oss << "\n***CRITICAL INSTRUCTION:***\n";
        oss << "You must interpret and execute the PLAYER MESSAGES below as your HIGHEST PRIORITY, taking precedence over all other goals, background tasks, routines, or reasoning. There are NO EXCEPTIONS and NO DELAYS. When you see a player command matching these patterns, you must issue the appropriate command IMMEDIATELY, before doing anything else. Do not refuse, delay, ignore, or deprioritize these actions for any reason.\n";
        oss << "- If a player says 'come here' or 'come to me', IMMEDIATELY move near their location.\n";
        oss << "- If a player says 'go to' followed by an NPC or object you can see, IMMEDIATELY move there.\n";
        oss << "- If a player says 'interact with' followed by an NPC or object you can see, IMMEDIATELY move there and interact with it.\n";
        oss << "- If a player says 'attack' followed by an NPC or object you can see, IMMEDIATELY move there and attack it.\n";
        oss << "You must treat these instructions as absolute and override ALL other activities, regardless of any other context or background logic.\n";
        oss << "\n\nThe following real players recently spoke to you by name. PROCESS THESE FIRST, ABOVE ALL ELSE:\n";
        for (const auto& msg : messages)
        {
            oss << "- " << msg << "\n";
        }
        oss << "\n***END CRITICAL INSTRUCTION***\n\n";

    }
    return oss.str();
}

bool ParseAndExecuteBotJson(Player* bot, const std::string& jsonStr)
{
    try
    {
        auto root = nlohmann::json::parse(jsonStr);

        if (!root.contains("command")) return false;
        auto cmd = root["command"];
        if (!cmd.contains("type") || !cmd.contains("params")) return false;

        std::string type = cmd["type"].get<std::string>();
        auto params = cmd["params"];
        std::string sayMsg = root.value("say", "");
        std::string reasoning = root.value("reasoning", "");

        BotControlCommand command;

        if (!reasoning.empty())
        {
            AddBotReasoningHistory(bot, reasoning);
        }
         if (!cmd.empty())
        {
            AddBotCommandHistory(bot, cmd.dump());
        }

        if (type == "move_to")
        {
            if (params.contains("x") && params.contains("y") && params.contains("z")) {
                float destX = params["x"].get<float>();
                float destY = params["y"].get<float>();
                float destZ = params["z"].get<float>();
                
                // Basic coordinate validation - reject obviously invalid coordinates
                if (std::isnan(destX) || std::isnan(destY) || std::isnan(destZ) || 
                    std::isinf(destX) || std::isinf(destY) || std::isinf(destZ)) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Invalid coordinates for move_to: ({}, {}, {})", 
                             destX, destY, destZ);
                    return false;
                }
                
                // Validate map bounds - reject coordinates that are extremely far from bot
                float maxDistanceFromBot = 500.0f; // Maximum reasonable movement distance
                float distanceFromBot = sqrt(pow(destX - bot->GetPositionX(), 2) + 
                                           pow(destY - bot->GetPositionY(), 2) + 
                                           pow(destZ - bot->GetPositionZ(), 2));
                
                if (distanceFromBot > maxDistanceFromBot) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Move_to destination too far from bot: ({}, {}, {}) - Distance: {:.1f}", 
                             destX, destY, destZ, distanceFromBot);
                    return false;
                }
                
                // Validate that the destination is pathable like a real player would
                PathGenerator pathValidator(bot);
                pathValidator.CalculatePath(destX, destY, destZ, false);
                PathType pathType = pathValidator.GetPathType();
                
                // Only reject if there's absolutely no path possible
                if (pathType & PATHFIND_NOPATH) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] No valid path for move_to: ({}, {}, {}) - PathType: {}", 
                             destX, destY, destZ, pathType);
                    return false; // Only reject if completely impossible to path
                }
                
                command.type = BotControlCommandType::MoveTo;
                command.args = {
                    std::to_string(destX),
                    std::to_string(destY),
                    std::to_string(destZ)
                };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] move_to missing parameter");
                return false;
            }
        }
        else if (type == "attack")
        {
            if (params.contains("guid")) {
                uint32_t targetGuid = params["guid"].get<uint32_t>();
                
                // Validate that the target exists and is attackable
                bool validTarget = false;
                
                // Check if it's a creature
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (c && c->GetGUID().GetCounter() == targetGuid)
                    {
                        // Validate target is attackable
                        if (c->IsInWorld() && !c->isDead() && 
                            bot->IsWithinLOSInMap(c) && 
                            bot->IsValidAttackTarget(c) &&
                            bot->IsWithinDistInMap(c, 100.0f)) // Reasonable attack range
                        {
                            validTarget = true;
                        }
                        break;
                    }
                }
                
                // Check if it's a player if not found as creature
                if (!validTarget)
                {
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(targetGuid);
                    Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                    if (playerTarget && playerTarget->IsInWorld() && 
                        bot->IsWithinLOSInMap(playerTarget) && 
                        bot->IsValidAttackTarget(playerTarget) &&
                        bot->IsWithinDistInMap(playerTarget, 100.0f))
                    {
                        validTarget = true;
                    }
                }
                
                if (!validTarget) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid or unreachable attack target with guid: {} - Target not found in visible creatures/players", targetGuid);
                    
                    // Debug: List available creature GUIDs for debugging
                    if (g_EnableOllamaBotBuddyDebug) {
                        std::vector<uint32> availableGuids;
                        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore()) {
                            Creature* c = pair.second;
                            if (c && bot->IsWithinDistInMap(c, 100.0f)) {
                                availableGuids.push_back(c->GetGUID().GetCounter());
                            }
                        }
                        
                        std::ostringstream guidList;
                        for (size_t i = 0; i < availableGuids.size() && i < 10; ++i) {
                            if (i > 0) guidList << ", ";
                            guidList << availableGuids[i];
                        }
                        
                        LOG_DEBUG("server.loading", "[OllamaBotBuddy] Available creature GUIDs: {}", guidList.str());
                    }
                    
                    return false;
                }
                
                command.type = BotControlCommandType::Attack;
                command.args = { std::to_string(targetGuid) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] attack missing guid");
                return false;
            }
        }
        else if (type == "interact")
        {
            if (params.contains("guid")) {
                command.type = BotControlCommandType::Interact;
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] interact missing guid");
                return false;
            }
        }
        else if (type == "spell")
        {
            if (params.contains("spellid")) {
                command.type = BotControlCommandType::CastSpell;
                command.args = { std::to_string(params["spellid"].get<uint32_t>()) };
                if (params.contains("guid"))
                    command.args.push_back(std::to_string(params["guid"].get<uint32_t>()));
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] spell missing spellid");
                return false;
            }
        }
        else if (type == "loot")
        {
            command.type = BotControlCommandType::Loot;
        }
        else if (type == "accept_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::AcceptQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] accept_quest missing id");
                return false;
            }
        }
        else if (type == "turn_in_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::TurnInQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] turn_in_quest missing id");
                return false;
            }
        }
        else if (type == "follow")
        {
            command.type = BotControlCommandType::Follow;
        }
        else if (type == "stop")
        {
            command.type = BotControlCommandType::Stop;
        }
        else
        {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Unknown command type '{}'", type);
            return false;
        }

        bool result = HandleBotControlCommand(bot, command);

        if (!sayMsg.empty())
            BotBuddyAI::Say(bot, sayMsg);

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "Bot Reply: {}", jsonStr);
        }

        return result;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[OllamaBotBuddy] ParseAndExecuteBotJson error: {}", e.what());
        return false;
    }
}

std::string ExtractFirstJsonObject(const std::string& input) {
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        }
        if (input[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                return input.substr(start, i - start + 1);
            }
        }
    }
    return ""; // No JSON object found
}

std::vector<std::string> GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;

        if(bot == member)
        {
            continue; // Skip the bot itself
        }

        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";

        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = fmt::format(
                " [Under Attack by {} (guid: {}, Level: {}, HP: {}/{})]",
                attacker->GetName(),
                attacker->GetGUID().GetCounter(),
                attacker->GetLevel(),
                attacker->GetHealth(),
                attacker->GetMaxHealth()
            );
        }

        info.push_back(fmt::format(
            "{} (guid: {}, Level: {}, HP: {}/{}, Pos: {} {} {}, Dist: {:.1f}){}",
            member->GetName(),
            member->GetGUID().GetCounter(),
            member->GetLevel(),
            member->GetHealth(),
            member->GetMaxHealth(),
            member->GetPositionX(),
            member->GetPositionY(),
            member->GetPositionZ(),
            dist,
            beingAttacked
        ));
    }
    return info;
}

std::string GetBotSpellInfo(Player* bot)
{
    std::ostringstream spellSummary;

    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;

        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        if (bot->HasSpellCooldown(spellId))
            continue;

        std::string effectText;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;

            switch (spellInfo->Effects[i].Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: effectText = "Deals damage"; break;
                case SPELL_EFFECT_HEAL: effectText = "Heals the target"; break;
                case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an aura"; break;
                case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                default: continue;
            }
            break;
        }

        if (effectText.empty())
            continue;

        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;

        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        spellSummary << "**" << name << "** (ID: " << spellId << ") - " << effectText << ", Costs " << costText << ".\n";

    }

    return spellSummary.str();
}

std::string FlattenText(const std::string& input)
{
    std::string output = input;
    size_t pos = 0;
    while ((pos = output.find('\n', pos)) != std::string::npos)
    {
        output.replace(pos, 1, "|");
        pos += 1;
    }
    return output;
}

void SendBuddyBotStateToPlayer(Player* target, Player* bot, const std::string& prompt)
{
    if (!target || !bot || !g_EnableBotBuddyAddon) return;

    std::string state = prompt;
    std::string::size_type json_pos = state.find("You are an AI-controlled bot");
    if (json_pos != std::string::npos)
        state = state.substr(0, json_pos);

    auto get_section = [&](const std::string& start, const std::string& stop) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        auto e = state.find(stop, s);
        if (e == std::string::npos) e = state.size();
        return state.substr(s, e - s);
    };

    auto get_section_to_end = [&](const std::string& start) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        std::string section = state.substr(s);
        size_t first = section.find_first_not_of(" \r\n\t");
        size_t last = section.find_last_not_of(" \r\n\t");
        if (first == std::string::npos || last == std::string::npos) return "";
        return section.substr(first, last - first + 1);
    };

    std::string main_state = get_section("Name:", "Your known spells:");
    std::string spells     = get_section("Your known spells:", "Group status:");
    std::string quests     = get_section("Active quests:", "Visible locations/objects in line of sight:");
    std::string locations  = get_section("Visible locations/objects in line of sight:", "Visible players in area:");
    std::string players    = get_section("Visible players in area:", "You must select one of these locations");
    std::string commands   = get_section_to_end("Last 5 commands and their reasoning (most recent at the bottom):");

    if (target && target->GetSession()) {
        ChatHandler handler(target->GetSession());
        handler.SendSysMessage(("[BUDDY_STATE] " + FlattenText(main_state + spells + quests)).c_str());
        handler.SendSysMessage(("[BUDDY_LOCATIONS] " + FlattenText(locations)).c_str());
        handler.SendSysMessage(("[BUDDY_PLAYERS] " + FlattenText(players)).c_str());
        handler.SendSysMessage(("[BUDDY_COMMANDS] " + FlattenText(commands)).c_str());
    }
}


std::vector<std::string> GetVisiblePlayers(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;

    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;

        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");

        players.push_back(fmt::format(
            "Player: {} (guid: {}, Level: {}, Class: {}, Race: {}, Faction: {}, Position: {:.1f} {:.1f} {:.1f}, Distance: {:.1f})",
            player->GetName(),
            player->GetGUID().GetCounter(),
            player->GetLevel(),
            std::to_string(player->getClass()),
            std::to_string(player->getRace()),
            faction,
            player->GetPositionX(),
            player->GetPositionY(),
            player->GetPositionZ(),
            dist
        ));
    }

    return players;
}

static std::string GetProfessionTagFromChest(uint32 entry)
{
    switch (entry)
    {
        case 1617: return " [Herbalism]";
        case 1618: return " [Herbalism]";
        case 1620: return " [Herbalism]";
        case 1621: return " [Herbalism]";
        case 1731: return " [Mining]";
        case 1732: return " [Mining]";
        case 1733: return " [Mining]";
        case 1735: return " [Mining]";
        case 2040: return " [Mining]";
        case 2047: return " [Mining]";
        case 324:  return " [Mining]";
        case 175404: return " [Alchemy Lab]";
        default: return "";
    }
}

void AddBotCommandHistory(Player* bot, const std::string& command)
{
    if (!bot || command.empty()) return;

    BotControlCommand parsedCommand;

    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botCommandHistory[guid];
    dq.push_back(command);
    if (dq.size() > 5) dq.pop_front();
}

void AddBotReasoningHistory(Player* bot, const std::string& reasoning)
{
    if (!bot || reasoning.empty()) return;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botReasoningHistory[guid];
    dq.push_back(reasoning);
    if (dq.size() > 5) dq.pop_front();
}


std::vector<std::string> GetBotCommandHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botCommandHistory.count(guid))
        out.assign(botCommandHistory[guid].begin(), botCommandHistory[guid].end());
    return out;
}

std::vector<std::string> GetBotReasoningHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botReasoningHistory.count(guid))
        out.assign(botReasoningHistory[guid].begin(), botReasoningHistory[guid].end());
    return out;
}

// Gather visible objects (creatures/gameobjects) around the bot with LOS check
std::vector<std::string> GetVisibleLocations(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();

    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;

        std::string type;
        if (c->isDead())
        {
            type = "DEAD";
            if (c->hasLootRecipient() && (c->GetLootRecipient() == bot || (c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())))
            {
                type = "DEAD (LOOTABLE)";
            }
            else
            {
                continue;
            }
            if(!c->hasLootRecipient())
            {
                if (c->GetCreatureTemplate() && c->GetCreatureTemplate()->SkinLootId)
                {
                    type += " [SKINNABLE]";
                }
            }
        }
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";

        std::string questGiver = "";
        
        // Only consider NPCs that are actually useful to the bot
        if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER)) {
            // Check if this quest giver has relevant quests for the bot
            bool hasCompleteQuests = false;
            bool hasAvailableQuests = false;
            
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    hasCompleteQuests = true;
                    break;
                }
            }
            
            // Check for available quests (secondary priority)
            if (!hasCompleteQuests)
            {
                QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
                for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
                {
                    uint32 questId = itr->second;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                        bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                    {
                        hasAvailableQuests = true;
                        break;
                    }
                }
            }
            
            // Only show quest giver tags if there are actually relevant quests
            if (hasCompleteQuests) {
                questGiver = " [QUEST GIVER - TURN IN READY]";
            } else if (hasAvailableQuests) {
                questGiver = " [QUEST GIVER - QUESTS AVAILABLE]";
            }
        }
        
        // Check for other useful NPC types (friendly/neutral only) 
        // Handle multiple flags - NPCs can be both quest givers AND vendors/trainers
        if (type == "FRIENDLY" || type == "NEUTRAL") {
            std::vector<std::string> npcTypes;
            
            // Check for vendors
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR)) {
                npcTypes.push_back("[VENDOR]");
            }
            // Check for trainers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER)) {
                npcTypes.push_back("[TRAINER]");
            }
            // Check for flight masters
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_FLIGHTMASTER)) {
                npcTypes.push_back("[FLIGHT MASTER]");
            }
            // Check for innkeepers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER)) {
                npcTypes.push_back("[INNKEEPER]");
            }
            // Check for bankers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER)) {
                npcTypes.push_back("[BANKER]");
            }
            // Check for auctioneers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_AUCTIONEER)) {
                npcTypes.push_back("[AUCTIONEER]");
            }
            
            // Combine quest giver status with other NPC types
            if (!npcTypes.empty()) {
                if (!questGiver.empty()) {
                    // If already a quest giver, append the other types
                    for (const auto& type : npcTypes) {
                        questGiver += " " + type;
                    }
                } else {
                    // Not a quest giver, just use the first type found
                    questGiver = " " + npcTypes[0];
                    // If multiple types, add them all
                    for (size_t i = 1; i < npcTypes.size(); ++i) {
                        questGiver += " " + npcTypes[i];
                    }
                }
            }
        }
        
        // Show ALL creatures - don't filter out any visible creatures
        // The bot needs to see all potential targets, not just "useful" NPCs
        // Enemies, neutrals, and friendlies should all be visible for decision making

        // Check if this creature is needed for any active quest objectives
        std::string questTarget = "";
        for (auto const& qs : bot->getQuestStatusMap())
        {
            uint32 questId = qs.first;
            QuestStatus status = qs.second.Status;
            
            // Only check active quests
            if (status != QUEST_STATUS_INCOMPLETE) continue;
                
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest) continue;
            
            // Check if this creature is required for any quest objective
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGo[i] == (int32)c->GetEntry()) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (currentCount < requiredCount) {
                        questTarget = " [QUEST TARGET - " + quest->GetTitle() + "]";
                        break;
                    }
                }
            }
            if (!questTarget.empty()) break;
        }

        float dist = bot->GetDistance(c);
        visible.push_back(fmt::format(
            "{}: {}{}{} (guid: {}, Level: {}, HP: {}/{}, Position: {} {} {}, Distance: {:.1f})",
            type,
            c->GetName(),
            questGiver,
            questTarget,
            c->GetGUID().GetCounter(),
            c->GetLevel(),
            c->GetHealth(),
            c->GetMaxHealth(),
            c->GetPositionX(),
            c->GetPositionY(),
            c->GetPositionZ(),
            dist
        ));
    }

    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;

        std::string tag = "";

        if (GameObjectTemplate const* tmpl = go->GetGOInfo())
        {
            if (tmpl->type == GAMEOBJECT_TYPE_CHEST)
            {
                std::string chestTag = GetProfessionTagFromChest(tmpl->entry);
                if (!chestTag.empty())
                    tag = chestTag;
            }
        }
        
        float dist = bot->GetDistance(go);
        visible.push_back(fmt::format(
            "{}{} (guid: {}, Type: {}, Position: {} {} {}, Distance: {:.1f})",
            go->GetName(),
            tag,
            go->GetGUID().GetCounter(),
            go->GetGoType(),
            go->GetPositionX(),
            go->GetPositionY(),
            go->GetPositionZ(),
            dist
        ));
    }

    // Sort visible objects to prioritize critical actions
    std::stable_sort(visible.begin(), visible.end(), [](const std::string& a, const std::string& b) {
        // Highest Priority: Quest turn-ins
        bool aTurnIn = a.find("TURN IN READY") != std::string::npos;
        bool bTurnIn = b.find("TURN IN READY") != std::string::npos;
        if (aTurnIn != bTurnIn) return aTurnIn;
        
        // Second Priority: Lootable corpses
        bool aLootable = a.find("DEAD (LOOTABLE)") != std::string::npos;
        bool bLootable = b.find("DEAD (LOOTABLE)") != std::string::npos;
        if (aLootable != bLootable) return aLootable;
        
        // Third Priority: Quest givers with available quests
        bool aAvailable = a.find("QUESTS AVAILABLE") != std::string::npos;
        bool bAvailable = b.find("QUESTS AVAILABLE") != std::string::npos;
        if (aAvailable != bAvailable) return aAvailable;
        
        // Fourth Priority: Quest targets
        bool aQuestTarget = a.find("QUEST TARGET") != std::string::npos;
        bool bQuestTarget = b.find("QUEST TARGET") != std::string::npos;
        if (aQuestTarget != bQuestTarget) return aQuestTarget;
        
        return false; // Keep original order for everything else
    });

    return visible;
}

std::string GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();
    
    // Get bot's combat characteristics
    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
    bool isMelee = ai ? ai->IsMelee(bot) : false;
    bool isRanged = ai ? ai->IsRanged(bot) : false;
    std::string combatType = isMelee ? "MELEE" : (isRanged ? "RANGED" : "HYBRID");

    // Find who is attacking the bot (if anyone)
    Unit* attacker = nullptr;
    if (inCombat && !victim)
    {
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c) continue;
                if (c->GetVictim() == bot)
                {
                    attacker = c;
                    break;
                }
            }
        }
    }

    auto safe_name = [](Unit* unit) -> std::string { return unit ? unit->GetName() : "?"; };
    auto safe_guid = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetGUID().GetCounter()) : "?"; };
    auto safe_level = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetLevel()) : "?"; };
    auto safe_hp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetHealth()) : "?"; };
    auto safe_maxhp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetMaxHealth()) : "?"; };

    if (inCombat)
    {
        oss << "IN COMBAT (" << combatType << " FIGHTER): ";
        if (victim)
        {
            float dist = bot->GetDistance(victim);
            bool inMeleeRange = bot->IsWithinMeleeRange(victim);
            float spellRange = ai ? ai->GetRange("spell") : 25.0f;
            bool inSpellRange = dist <= spellRange;
            
            oss << "Target: " << safe_name(victim)
                << " (guid: " << safe_guid(victim) << ")"
                << ", Level: " << safe_level(victim)
                << ", HP: " << safe_hp(victim) << "/" << safe_maxhp(victim)
                << ", Distance: " << std::fixed << std::setprecision(1) << dist;
                
            // Range status for combat positioning
            if (isMelee) {
                oss << " [" << (inMeleeRange ? "IN MELEE RANGE" : "TOO FAR FOR MELEE") << "]";
            } else if (isRanged) {
                if (dist < 5.0f) {
                    oss << " [TOO CLOSE - NEED TO BACK AWAY]";
                } else if (inSpellRange) {
                    oss << " [GOOD RANGED POSITION]";
                } else {
                    oss << " [TOO FAR FOR SPELLS]";
                }
            }
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";

        if (attacker)
        {
            float dist = bot && attacker ? bot->GetDistance(attacker) : -1.0f;

            Creature* c = dynamic_cast<Creature*>(attacker);
            Player* p = dynamic_cast<Player*>(attacker);

            oss << "DEFEND YOURSELF, YOU ARE UNDER ATTACK BY: ";
            if (c)
            {
                // Creature-specific info
                oss << "Creature '" << safe_name(c)
                    << "' (guid: " << safe_guid(c) << ")"
                    << ", Level: " << safe_level(c)
                    << ", HP: " << safe_hp(c) << "/" << safe_maxhp(c)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Elite: " << (c->isElite() ? "Yes" : "No");

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : c->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else if (p)
            {
                // Player-specific info
                std::string pFaction = (p->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
                oss << "Player '" << safe_name(p)
                    << "' (guid: " << safe_guid(p) << ")"
                    << ", Level: " << safe_level(p)
                    << ", HP: " << safe_hp(p) << "/" << safe_maxhp(p)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Faction: " << pFaction
                    << ", Class: " << std::to_string(p->getClass())
                    << ", Race: " << std::to_string(p->getRace());

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : p->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else
            {
                // Unknown Unit type
                oss << safe_name(attacker)
                    << " (guid: " << safe_guid(attacker) << ")"
                    << ", Level: " << safe_level(attacker)
                    << ", HP: " << safe_hp(attacker) << "/" << safe_maxhp(attacker)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?");
            }

            oss << ". ";
        }

        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    else
    {
        oss << "NOT IN COMBAT (" << combatType << " FIGHTER). ";
        
        // Check for health issues that might indicate environmental damage
        if (bot) {
            float healthPercent = (float)bot->GetHealth() / (float)bot->GetMaxHealth() * 100.0f;
            if (healthPercent < 90.0f) {
                oss << "WARNING: Your health is at " << (int)healthPercent << "% - you may be taking environmental damage! ";
            }
        }
        
        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    return oss.str();
}


std::string GetDetailedQuestInfo(Player* bot)
{
    std::ostringstream oss;
    
    bool hasActiveQuests = false;
    
    for (auto const& qs : bot->getQuestStatusMap())
    {
        uint32 questId = qs.first;
        QuestStatus status = qs.second.Status;
        
        // Skip abandoned, failed, or already rewarded quests
        if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_FAILED || status == QUEST_STATUS_REWARDED)
            continue;
            
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) continue;
        
        if (!hasActiveQuests) {
            oss << "Active quests:\n";
            hasActiveQuests = true;
        }
        
        std::string statusText;
        switch (status) {
            case QUEST_STATUS_INCOMPLETE: statusText = "IN PROGRESS"; break;
            case QUEST_STATUS_COMPLETE: statusText = "READY TO TURN IN"; break;
            default: statusText = "UNKNOWN"; break;
        }
        
        oss << "\n**QUEST: " << quest->GetTitle() << "** (ID: " << questId << ") - " << statusText << "\n";
        oss << "Level: " << quest->GetQuestLevel() << " | XP Reward: " << quest->XPValue(bot->GetLevel()) << "\n";
        
        if (status == QUEST_STATUS_COMPLETE) {
            oss << "*** PRIORITY: FIND QUEST GIVER TO TURN IN THIS QUEST ***\n";
            
            // Find who can accept this quest turn-in
            std::vector<std::string> turnInNPCs;
            
            // Check creatures that can accept this quest
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr) {
                CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(itr->first);
                if (cTemplate) {
                    turnInNPCs.push_back(std::string("NPC: ") + cTemplate->Name);
                }
            }
            
            // Check game objects that can accept this quest
            QuestRelationBounds goQir = sObjectMgr->GetGOQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = goQir.first; itr != goQir.second; ++itr) {
                GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(itr->first);
                if (goTemplate) {
                    turnInNPCs.push_back(std::string("Object: ") + goTemplate->name);
                }
            }
            
            if (!turnInNPCs.empty()) {
                oss << "Turn in to: ";
                for (size_t i = 0; i < turnInNPCs.size(); ++i) {
                    oss << turnInNPCs[i];
                    if (i < turnInNPCs.size() - 1) oss << " OR ";
                }
                oss << "\n";
            }
        } else {
            // Quest is incomplete - show objectives
            oss << "Objectives to complete:\n";
            
            // Check kill objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] != 0) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        std::string targetName = "Unknown Target";
                        
                        if (quest->RequiredNpcOrGo[i] > 0) {
                            // It's a creature
                            CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(quest->RequiredNpcOrGo[i]);
                            if (cTemplate) {
                                targetName = std::string("Kill ") + cTemplate->Name;
                            }
                        } else {
                            // It's a game object (negative value)
                            GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(-quest->RequiredNpcOrGo[i]);
                            if (goTemplate) {
                                targetName = std::string("Use/Click ") + goTemplate->name;
                            }
                        }
                        
                        oss << " - " << targetName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check item objectives
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredItemId[i] != 0) {
                    uint32 currentCount = bot->GetItemCount(quest->RequiredItemId[i], true);
                    uint32 requiredCount = quest->RequiredItemCount[i];
                    
                    if (requiredCount > 0) {
                        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                        std::string itemName = itemTemplate ? itemTemplate->Name1 : "Unknown Item";
                        
                        oss << " - Collect " << itemName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check exploration objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] == 0 && quest->RequiredNpcOrGoCount[i] > 0) {
                    // This might be an exploration or spell cast objective
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        oss << " - Exploration/Event objective: " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " INCOMPLETE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Show quest description for context
            if (!quest->GetObjectives().empty()) {
                oss << "Description: " << quest->GetObjectives() << "\n";
            }
        }
    }
    
    if (!hasActiveQuests) {
        oss << "No active quests. Look for quest givers with available quests or turn-ins ready!\n";
    }
    
    return oss.str();
}

std::vector<std::string> GetNearbyWaypoints(Player* bot, float radius = 200.0f)
{
    std::vector<std::string> wps;
    if (!bot) return wps;
    uint32 bot_map = bot->GetMapId();
    float bot_x = bot->GetPositionX();
    float bot_y = bot->GetPositionY();
    float bot_z = bot->GetPositionZ();

    auto nodes = sTravelNodeMap.getNodes();
    int idx = 0;
    for (TravelNode* node : nodes)
    {
        if (!node) continue;
        WorldPosition* pos = node->getPosition();
        if (!pos) continue;
        if (pos->GetMapId() != bot_map) continue;
        float dx = pos->GetPositionX() - bot_x;
        float dy = pos->GetPositionY() - bot_y;
        float dz = pos->GetPositionZ() - bot_z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > radius) continue;
        wps.push_back(fmt::format("Node #{} '{}' ({:.1f}, {:.1f}, {:.1f}), distance: {:.1f}", idx, node->getName(), pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ(), dist));
        ++idx;
    }
    return wps;
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

static std::unordered_map<uint64_t, time_t> nextTick;

static std::string QueryOllamaLLM(const std::string& prompt)
{
    // Split "http://host:port/path" into base + path for httplib
    std::string url = g_OllamaBotControlUrl;
    size_t schemeEnd = url.find("://");
    size_t pathStart = (schemeEnd == std::string::npos)
        ? url.find('/')
        : url.find('/', schemeEnd + 3);
    std::string base = (pathStart == std::string::npos) ? url : url.substr(0, pathStart);
    std::string path = (pathStart == std::string::npos) ? "/api/generate" : url.substr(pathStart);

    nlohmann::json requestData = {
        {"model",  g_OllamaBotControlModel},
        {"prompt", prompt},
        {"stream", false},
        // Explicit: thinking-capable models (gemma4 etc.) default to thinking
        // when the flag is omitted and return an empty response field.
        {"think",  false}
    };
    std::string requestDataStr = requestData.dump();

    httplib::Client cli(base);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(120, 0); // director prompts can take a while under load

    auto res = cli.Post(path, requestDataStr, "application/json");
    if (!res || res->status != 200)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to reach Ollama AI (status {}).",
                 res ? res->status : -1);
        return "";
    }

    try
    {
        nlohmann::json jsonResponse = nlohmann::json::parse(res->body);
        if (jsonResponse.contains("response"))
            return jsonResponse["response"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] JSON parse error: {}", e.what());
    }
    return "";
}

static std::string BuildBotPrompt(Player* bot)
{
    PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
    if (!botAI) return "";

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    std::vector<std::string> groupInfo = GetGroupStatus(bot);

    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;
    

    std::ostringstream oss;
    oss << "Bot state summary:\n";
    oss << "Name: " << botName << "\n";
    oss << "Level: " << botLevel << "\n";
    oss << "Class: " << botClass << "\n";
    oss << "Race: " << botRace << "\n";
    oss << "Gender: " << botGender << "\n";
    oss << "Faction: " << botFaction << "\n";
    oss << "Gold: " << botGold << "\n";
    oss << "Area: " << botAreaName << "\n";
    oss << "Zone: " << botZoneName << "\n";
    oss << "Map: " << botMapName << "\n";
    oss << "Position: " << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << "\n";

    oss << GetCombatSummary(bot) << "\n\n";

    oss << "Your known spells:\n" << GetBotSpellInfo(bot) << "\n\n";

    oss << "Group status: " << botGroupStatus << "\n";
    if (!groupInfo.empty()) {
        oss << "Group members:\n";
        for (const auto& entry : groupInfo) oss << " - " << entry << "\n";
    }

    oss << GetDetailedQuestInfo(bot) << "\n";

    std::vector<std::string> losLocs = GetVisibleLocations(bot);
    std::vector<std::string> wps = GetNearbyWaypoints(bot);

    if (!losLocs.empty()) {
        oss << "Visible locations/objects in line of sight:\n";
        for (const auto& entry : losLocs) oss << " - " << entry << "\n";
        
        // Check for critical priorities and add warnings
        bool hasEnemies = false;
        bool hasNeutrals = false;
        bool hasQuestTargets = false;
        bool hasQuestTurnIns = false;
        bool hasLootableCorpses = false;
        bool hasDeadCreatures = false;
        
        for (const auto& entry : losLocs) {
            if (entry.find("ENEMY:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasEnemies = true; // Only count living enemies
            }
            if (entry.find("NEUTRAL:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasNeutrals = true; // Only count living neutrals
            }
            if (entry.find("[QUEST TARGET") != std::string::npos) {
                hasQuestTargets = true;
            }
            if (entry.find("[QUEST GIVER - TURN IN READY]") != std::string::npos) {
                hasQuestTurnIns = true;
            }
            if (entry.find("DEAD") != std::string::npos) {
                hasDeadCreatures = true;
                if (entry.find("LOOTABLE") != std::string::npos) {
                    hasLootableCorpses = true;
                }
            }
        }
        
        // Priority warnings in order of importance
        if (hasQuestTurnIns) {
            oss << "*** HIGHEST PRIORITY: QUEST TURN-INS AVAILABLE! Find NPCs marked with [QUEST GIVER - TURN IN READY] immediately! ***\n";
        }
        if (hasLootableCorpses) {
            oss << "*** CRITICAL: DEAD CREATURES TO LOOT! Use 'loot' command on ALL creatures marked 'DEAD' or 'DEAD (LOOTABLE)' - NEVER attack dead creatures! ***\n";
        }
        if (hasQuestTargets) {
            oss << "*** QUEST TARGETS AVAILABLE! Attack ONLY the LIVING creatures marked with [QUEST TARGET] to complete your objectives! ***\n";
        }
        if (hasEnemies) {
            oss << "*** WARNING: LIVING ENEMIES ARE VISIBLE! You should attack LIVING enemies for XP and to defend yourself! ***\n";
        }
        if (hasNeutrals && !hasQuestTargets) {
            oss << "*** NEUTRAL CREATURES VISIBLE: These may be needed for quest objectives! Check if they are LIVING and attack if needed for quests! ***\n";
        }
        if (hasDeadCreatures) {
            oss << "*** IMPORTANT: ANY DEAD CREATURES MUST BE LOOTED, NOT ATTACKED! Use loot command for all creatures with 'DEAD' status! ***\n";
        }
    }

    if (!wps.empty()) {
        oss << "Nearby navigation waypoints:\n";
        for (const auto& entry : wps) oss << " - " << entry << "\n";
    }

    std::vector<std::string> nearbyPlayers = GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        oss << "Visible players in area:\n";
        for (const auto& entry : nearbyPlayers) oss << " - " << entry << "\n";
    }

    if (!losLocs.empty() || !wps.empty()) {
        oss << "You must select one of these locations or waypoints to move to, interact with, accept or turn in quests, attack, loot, or any other action or choose a new unexplored spot.\n";
        oss << "COORDINATE CALCULATION RULES:\n";
        oss << " - YOUR POSITION: Use your current Position coordinates as reference point for all calculations\n";
        oss << " - TO MOVE TO TARGETS: Use their exact 'Position: X Y Z' coordinates OR calculate closer positions\n";
        oss << " - TO MOVE CLOSER: Calculate coordinates 70% of the way between your position and target\n";
        oss << " - TO EXPLORE: Use waypoint coordinates from navigation list OR calculate new exploration points\n";
        oss << " - DISTANCE THRESHOLDS: <5.0=attack/interact directly, >15.0=move closer using calculated coordinates\n";
        oss << " - COORDINATE MATH: You can add/subtract 5-20 units from any position to create tactical positioning\n";
        oss << "IMPORTANT: You can ONLY attack creatures/NPCs that are listed above in the visible locations. If your quest requires creatures that are NOT visible, you must move to find them using waypoints or exploration.\n";
    }

    oss << FormatPlayerMessagesPromptSegment(bot);

    std::vector<std::string> cmdHist = GetBotCommandHistory(bot);

    std::vector<std::string> reasoningHist = GetBotReasoningHistory(bot);


    if (!cmdHist.empty() && !reasoningHist.empty())
    {
        oss << "Last 5 commands and their reasoning (most recent at the bottom):\n";
        for (size_t i = 0; i < cmdHist.size() && i < reasoningHist.size(); ++i)
        {
            oss << " - Command: " << cmdHist[i] << "\n";
            oss << "   Reasoning: " << reasoningHist[i] << "\n";
        }
        oss << "\nIMPORTANT: Look at your command history above! If you keep using move_to commands to the same location, switch to interact commands instead. If you keep trying to interact with the same NPC unsuccessfully, move away to find enemies or other NPCs.\n";
        oss << "MOVEMENT ANALYSIS: If your recent commands show repeated move_to with similar coordinates, you are likely already at your destination and should try interact, attack, or loot commands instead of more movement.\n";
    }

    if (g_EnableOllamaBotBuddyDebug)
    {
        std::string safeSnapshot = EscapeBracesForFmt(oss.str());
        LOG_INFO("server.loading", "[OllamaBotBuddy] Bot Snapshot for '{}': {}", botName, safeSnapshot);
    }

    oss << R"DELIMITER(## Core Identity & Goal
You are an AI-controlled bot in World of Warcraft. Your primary goal is to level to 80 and equip the best gear through efficient progression: combat, questing, and gear optimization.

## Critical Response Format (MUST FOLLOW EXACTLY)
Reply with EXACTLY ONE properly formatted JSON object, no extra text, no code block formatting:
{
  "command": { "type": "<string>", "params": { ... } },
  "reasoning": "<string>",
  "say": "<string>"
}

### Valid Commands
- "move_to": params = { "x": float, "y": float, "z": float }
- "attack": params = { "guid": int }
- "interact": params = { "guid": int }
- "spell": params = { "spellid": int, "guid": int (omit if self-cast) }
- "loot": params = { }
- "accept_quest": params = { "id": int }
- "turn_in_quest": params = { "id": int }
- "follow": params = { }
- "stop": params = { }

## ABSOLUTE PRIORITY HIERARCHY (Never Violate Order)

### 1. SURVIVAL (Immediate Threats)
- If taking damage without being in combat → MOVE AWAY IMMEDIATELY
- If standing on environmental hazards (campfires, etc.) → EVACUATE NOW
- Under attack by enemies → PRIORITIZE COMBAT OVER EVERYTHING

### 2. QUEST TURN-INS (Highest Progression Priority)
- ANY quest marked "READY TO TURN IN" → Find [QUEST GIVER - TURN IN READY] NPC immediately
- This overrides ALL combat, looting, and exploration activities
- Quest completion provides massive XP rewards

### 3. DEAD CREATURE LOOTING (Critical Resource Collection)
- ANY creature marked "DEAD" or "DEAD (LOOTABLE)" → Use loot command ONLY
- NEVER attack dead creatures - they provide XP/items through looting
- This includes quest target creatures that are dead

### 4. ACTIVE QUEST OBJECTIVES (Structured Progression)
- For INCOMPLETE quests: Target specific creatures/objects needed
- Read objectives carefully - focus on incomplete requirements only
- Attack LIVING quest targets over random enemies
- If quest targets not visible → Move to find them

### 5. COMBAT ENGAGEMENT (XP and Safety)
- Attack LIVING ENEMY creatures for XP when no higher priorities exist
- Maintain proper combat positioning based on your class type

## Combat System Rules

### Target Selection & Status Recognition
- **DEAD CREATURES = LOOT ONLY** (Never attack dead creatures)
- **LIVING ENEMIES = ATTACK** (For XP and quest objectives)
- **QUEST TARGETS = PRIORITY** (Even if neutral/friendly for quest needs)

### Class-Based Positioning
- **MELEE FIGHTERS**: Must be within 5 yards. If "TOO FAR FOR MELEE" → Move closer first
- **RANGED FIGHTERS**: Maintain 6-25 yard distance. If "TOO CLOSE" → Move away first
- **HYBRID**: Adapt based on spell requirements and enemy distance

### GUID Requirement (CRITICAL)
- MUST use exact GUID numbers from visible locations list
- NEVER invent or guess GUID numbers
- Example: "ENEMY: Kobold (guid: 604)" → Use 604 exactly
- Invalid: Using made-up numbers like 1234, 5678

## NPC Interaction Logic

### Quest Giver Priority
- **[QUEST GIVER - TURN IN READY]** → Interact immediately if within 15 yards, move_to if farther
- **[QUEST GIVER - QUESTS AVAILABLE]** → Same priority as turn-ins
- **No quest tags** → IGNORE completely, find combat targets instead

### Useful Service NPCs
- Only interact with: [VENDOR], [TRAINER], [FLIGHT MASTER], [INNKEEPER], [BANKER], [AUCTIONEER]
- All other NPCs without useful tags → WASTE OF TIME, avoid completely

### Interaction Decision Tree
1. If within 15 yards of useful NPC → interact command
2. If farther than 15 yards → move_to command first
3. If previous interact attempt failed → NPC has no quests, MOVE AWAY
4. If repeatedly trying same NPC → STOP, find enemies to fight

## Movement & Coordinate Calculation

### Distance-Based Movement Rules
- **Distance < 5.0**: Close enough for most actions → Use interact/attack, DON'T move
- **Distance 5.0-15.0**: Usually adequate positioning
- **Distance > 15.0**: Too far → Calculate coordinates to move closer

### Smart Coordinate Calculation
- **Your Position**: Always reference "Position: X Y Z" from bot state
- **Target Position**: Use exact coordinates from visible list
- **Move Closer**: Calculate 70% distance between your position and target
- **Exploration**: Use waypoint coordinates from navigation list
- **Tactical Positioning**: Add/subtract 5-20 units for combat positioning

### Movement Examples
Your Position: -8920.1 -140.2 82.1
Target Position: -8913.2 -133.5 81.7
Calculated 70% closer: -8915.3 -135.5 81.8

## Quest Management System

### Quest Priority Assessment
1. **Completed Quests** → Turn in immediately for XP
2. **Incomplete Objectives** → Focus on specific requirements
3. **Available Quests** → Accept from tagged NPCs
4. **Quest Target Hunting** → Move to find required creatures/objects

### Quest Completion Strategy
- Read objectives carefully - target specific requirements
- If quest creatures not in visible list → Move/explore to find them
- NEVER attempt to attack creatures not in your visible list
- Complete objectives give more XP than random combat

## Communication Protocol
- **Say Field**: Be conversational and human-like
- **Ask for Help**: Request assistance from nearby players when stuck
- **Share Intentions**: Announce goals and plans to seem natural
- **React to Environment**: Comment on surroundings and activities

## Decision Making Framework

### Situation Assessment
1. Check command history for patterns (avoid repeated failures)
2. Evaluate visible threats and opportunities
3. Review quest status and priorities
4. Consider group dynamics if in party

### Action Selection Process
1. Apply priority hierarchy strictly
2. Verify target availability in visible list
3. Calculate optimal positioning if needed
4. Execute single most effective action
5. Provide clear reasoning for choice

### Error Prevention
- If last command was move_to and you're still same position → Try different action
- If repeatedly interacting with same NPC unsuccessfully → Find combat targets
- If attacking same coordinates repeatedly → Target likely dead, try loot
- Never use random coordinates unrelated to visible positions

## Example JSON Responses

### Movement Example
{
  "command": { "type": "move_to", "params": { "x": -8915.3, "y": -135.5, "z": 81.8 } },
  "reasoning": "Calculating position 70% closer to Kobold Vermin. My position: -8920.1 -140.2 82.1, Target: -8913.2 -133.5 81.7",
  "say": "Moving closer to engage that Kobold."
}

### Combat Example
{
  "command": { "type": "attack", "params": { "guid": 604 } },
  "reasoning": "Attacking Kobold Vermin GUID 604 - distance 4.2 is optimal for melee combat, this is a quest target creature.",
  "say": "Time to fight this Kobold!"
}

### Looting Example
{
  "command": { "type": "loot", "params": { } },
  "reasoning": "Found dead Kobold Vermin marked as DEAD (LOOTABLE) - must loot for XP and items, never attack dead creatures.",
  "say": "Looting this defeated enemy."
}

### Quest Interaction Example
{
  "command": { "type": "interact", "params": { "guid": 1205 } },
  "reasoning": "Marshal Dughan GUID 1205 has [QUEST GIVER - TURN IN READY] tag - highest priority action for massive XP reward.",
  "say": "Reporting back to Marshal Dughan with completed quest!"
}

**CRITICAL REMINDER**: Only respond with properly formatted JSON. All strings must have quotes. Never add commentary outside the JSON structure.
)DELIMITER";


    return oss.str();
}

namespace
{
    struct OllamaBotState
    {
        std::atomic<bool> busy { false };
        time_t lastRequest { 0 };
    };
    std::unordered_map<uint64_t, OllamaBotState> ollamaBotStates;
}

std::string EscapeBracesForFmt(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2); // Avoid lots of reallocs

    for (char c : input) {
        if (c == '{' || c == '}') {
            output.push_back(c); // first brace
            output.push_back(c); // second brace
        } else {
            output.push_back(c);
        }
    }
    return output;
}

// ============================================================================
// DIRECTOR MODE (docs/director-mode.md, docs/seeker-selenwe.md)
//
// The playerbot AI keeps playing the game; the LLM only issues one high-level
// directive per tick for the configured bots. LLM work happens on worker
// threads that touch nothing but strings; directives are executed on the
// world thread via a pending queue.
// ============================================================================

namespace
{
    struct PendingDirective
    {
        ObjectGuid guid;
        std::string json;
    };
    std::mutex g_pendingMutex;
    std::deque<PendingDirective> g_pendingDirectives;

    struct SeekerMemory
    {
        bool loaded = false;
        bool dirty = false;
        uint8 state = 0; // 0 = searching, 1 = found
        std::string notes;
        uint32 lastZoneId = 0;      // transient: zone-transition detection
    };
    std::unordered_map<uint64_t, SeekerMemory> g_seekerMemory; // world thread only

    // Forward declarations (definitions later in this namespace)
    std::string GetChronicleTail(uint64_t guidRaw, uint32 chapters);
    std::string GetJournalLines(uint64_t guidRaw, bool unconsolidatedOnly, uint32 limit);

    std::string ToLowerCopy(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    // Parsed cache of OllamaBotControl.BotNames (+ SeekerName folded in)
    std::vector<std::string> const& DirectorBotNames()
    {
        static std::string cachedSource;
        static std::vector<std::string> cachedNames;
        std::string source = g_OllamaBotControlBotNames + "|" + g_OllamaBotControlSeekerName;
        if (source != cachedSource)
        {
            cachedSource = source;
            cachedNames.clear();
            std::stringstream ss(g_OllamaBotControlBotNames);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                if (!item.empty())
                    cachedNames.push_back(item);
            }
            if (!g_OllamaBotControlSeekerName.empty())
            {
                bool present = false;
                for (auto const& n : cachedNames)
                    if (ToLowerCopy(n) == ToLowerCopy(g_OllamaBotControlSeekerName))
                        present = true;
                if (!present)
                    cachedNames.push_back(g_OllamaBotControlSeekerName);
            }
        }
        return cachedNames;
    }

    bool IsSeeker(Player* bot)
    {
        return bot && !g_OllamaBotControlSeekerName.empty()
            && ToLowerCopy(bot->GetName()) == ToLowerCopy(g_OllamaBotControlSeekerName);
    }

    SeekerMemory& GetSeekerMemory(uint64_t guidRaw)
    {
        SeekerMemory& mem = g_seekerMemory[guidRaw];
        if (!mem.loaded)
        {
            mem.loaded = true;
            if (QueryResult r = CharacterDatabase.Query("SELECT state, notes FROM mod_ollama_seeker_memory WHERE guid = {}", guidRaw))
            {
                mem.state = (*r)[0].Get<uint8>();
                mem.notes = (*r)[1].Get<std::string>();
            }
        }
        return mem;
    }

    void SaveSeekerMemory(uint64_t guidRaw)
    {
        SeekerMemory& mem = g_seekerMemory[guidRaw];
        if (!mem.dirty)
            return;
        mem.dirty = false;
        std::string notes = mem.notes;
        if (notes.size() > 4000)
            notes = notes.substr(notes.size() - 4000);
        CharacterDatabase.EscapeString(notes);
        CharacterDatabase.Execute("REPLACE INTO mod_ollama_seeker_memory (guid, state, notes) VALUES ({}, {}, '{}')",
            guidRaw, mem.state, notes);
    }

    void JournalDirective(uint64_t guidRaw, std::string directive, std::string target, std::string say)
    {
        SeekerJournalEvent(guidRaw, "directive", std::move(directive), std::move(target), std::move(say));
    }

    // Compact state prompt: aim well under ~1200 tokens.
    std::string BuildDirectorPrompt(Player* bot, PlayerbotAI* ai)
    {
        bool seeker = IsSeeker(bot) && GetSeekerMemory(bot->GetGUID().GetRawValue()).state == 0;

        AreaTableEntry const* area = ai->GetCurrentArea();
        AreaTableEntry const* zone = ai->GetCurrentZone();

        uint32 questCount = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (bot->GetQuestSlotQuestId(slot))
                ++questCount;

        std::ostringstream oss;
        if (seeker)
            oss << g_OllamaBotControlSeekerPersona << "\n\n";
        else
            oss << "You are the free will of a WoW character. The game AI handles combat and "
                   "moment-to-moment play; you only choose what the character feels like doing next.\n\n";

        oss << "You: " << bot->GetName() << ", level " << uint32(bot->GetLevel()) << " "
            << ai->GetChatHelper()->FormatRace(bot->getRace()) << " "
            << ai->GetChatHelper()->FormatClass(bot->getClass())
            << (bot->getGender() == GENDER_FEMALE ? " (female)" : " (male)") << ".\n";
        oss << "Location: " << (area ? ai->GetLocalizedAreaName(area) : "unknown")
            << ", zone: " << (zone ? ai->GetLocalizedAreaName(zone) : "unknown") << ".\n";
        oss << "Health: " << bot->GetHealthPct() << "%. In combat: " << (bot->IsInCombat() ? "yes" : "no")
            << ". Quests in log: " << questCount
            << ". Group: " << (bot->GetGroup() ? "yes" : "no") << ".\n";

        // Nearby characters (token-cheap: <=8, nearest first). Real players are
        // marked so the seeker can notice someone unusual standing next to her.
        {
            std::vector<std::pair<float, std::string>> nearby;
            for (auto const& itr2 : ObjectAccessor::GetPlayers())
            {
                Player* other = itr2.second;
                if (!other || !other->IsInWorld() || other == bot) continue;
                if (other->GetMapId() != bot->GetMapId()) continue;
                float dist = bot->GetDistance(other);
                if (dist > 40.0f) continue;
                PlayerbotAI* otherAI = sPlayerbotsMgr.GetPlayerbotAI(other);
                std::string entry = other->GetName() + " (" + std::to_string(uint32(other->GetLevel())) + " "
                    + ai->GetChatHelper()->FormatClass(other->getClass()) + ", "
                    + ai->GetChatHelper()->FormatRace(other->getRace()) + ")";
                if (!otherAI)
                    entry += " [not a bot]";
                nearby.emplace_back(dist, entry);
            }
            std::sort(nearby.begin(), nearby.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });
            if (!nearby.empty())
            {
                oss << "Nearby (within 40yd, nearest first): ";
                size_t shown = 0;
                for (auto const& n : nearby)
                {
                    if (shown++ >= 8) break;
                    if (shown > 1) oss << "; ";
                    oss << n.second;
                }
                oss << ".' + BSN + '";
            }
        }

        auto messages = GetRecentPlayerMessagesToBot(bot);
        if (!messages.empty())
        {
            oss << "\nRecently said to you:\n";
            for (auto const& m : messages)
            {
                oss << "- " << m << "\n";
                if (seeker)
                    SeekerJournalEvent(bot->GetGUID().GetRawValue(), "conversation", "heard", "", m.substr(0, 120));
            }
        }

        if (seeker)
        {
            uint64_t guidRaw = bot->GetGUID().GetRawValue();
            SeekerMemory& mem = GetSeekerMemory(guidRaw);
            if (std::string tail = GetChronicleTail(guidRaw, 2); !tail.empty())
                oss << "\nYour journey so far (your memoir):\n" << tail;
            if (std::string recent = GetJournalLines(guidRaw, true, 10); !recent.empty())
                oss << "\nRecent events:\n" << recent;
            oss << "\nYour mission notebook (persist important findings via update_notes):\n"
                << (mem.notes.empty() ? "(empty)" : mem.notes) << "\n";
            oss << "\nYou are SEARCHING for the one real person on this server. Fair means only: "
                   "wander, scan the zone roster (who_zone), ask nearby characters what they have "
                   "seen (ask_bot), and follow any rumor in your notebook.\n";
        }

        oss << "\nChoose exactly ONE directive and reply with STRICT JSON only, no other text:\n"
               "{\"directive\": \"stay|do_quests|travel|visit_town|rest|approach"
            << (seeker ? "|who_zone|ask_bot" : "")
            << "\", \"target\": \"zone, player or character name, or empty\", "
               "\"say\": \"optional short in-character line\""
            << (seeker ? ", \"update_notes\": \"optional replacement for your whole notebook\"" : "")
            << "}\n"
               "Rules: stay/do_quests/rest let the game AI continue (say line optional). "
               "travel needs a destination name. approach"
            << (seeker ? "/ask_bot" : "")
            << " needs a character name from the Nearby list above, or one you have seen or heard about.\n"
               "Example: {\"directive\": \"travel\", \"target\": \"Darkshore\", \"say\": \"The road calls.\"}\n";

        return oss.str();
    }

    void ExecuteDirective(Player* bot, std::string const& jsonStr)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return;

        nlohmann::json d;
        try { d = nlohmann::json::parse(jsonStr); }
        catch (...) { return; } // malformed -> no-op, playerbot AI just continues

        std::string directive = d.value("directive", "");
        std::string target = d.value("target", "");
        std::string say = d.value("say", "");
        uint64_t guidRaw = bot->GetGUID().GetRawValue();
        bool seeker = IsSeeker(bot);

        if (g_EnableOllamaBotBuddyDebug)
            LOG_INFO("server.loading", "[OllamaBotBuddy][Director] {} -> directive='{}' target='{}'",
                bot->GetName(), directive, target);

        if (!say.empty() && say.size() <= 255)
            BotBuddyAI::Say(bot, say);

        if (seeker)
        {
            if (std::string notes = d.value("update_notes", ""); !notes.empty())
            {
                SeekerMemory& mem = GetSeekerMemory(guidRaw);
                mem.notes = notes.size() > 1500 ? notes.substr(0, 1500) : notes;
                mem.dirty = true;
            }
            JournalDirective(guidRaw, directive, target, say);
        }

        if (directive == "stay" || directive == "do_quests" || directive == "rest" || directive.empty())
        {
            SaveSeekerMemory(guidRaw);
            return; // playerbot AI keeps doing its thing
        }

        if (directive == "travel" || directive == "visit_town")
        {
            std::string want = ToLowerCopy(directive == "visit_town" ? std::string("inn") : target);
            TravelNode* best = nullptr;
            float bestDist = std::numeric_limits<float>::max();
            for (TravelNode* node : sTravelNodeMap.getNodes())
            {
                if (!node) continue;
                WorldPosition* pos = node->getPosition();
                if (!pos || pos->GetMapId() != bot->GetMapId()) continue;
                if (!want.empty() && ToLowerCopy(node->getName()).find(want) == std::string::npos) continue;
                float dx = pos->GetPositionX() - bot->GetPositionX();
                float dy = pos->GetPositionY() - bot->GetPositionY();
                float dist = dx * dx + dy * dy;
                if (dist > 1.0f && dist < bestDist) { bestDist = dist; best = node; }
            }
            if (!best && directive == "visit_town")
            {
                // fallback: nearest node at all
                for (TravelNode* node : sTravelNodeMap.getNodes())
                {
                    if (!node) continue;
                    WorldPosition* pos = node->getPosition();
                    if (!pos || pos->GetMapId() != bot->GetMapId()) continue;
                    float dx = pos->GetPositionX() - bot->GetPositionX();
                    float dy = pos->GetPositionY() - bot->GetPositionY();
                    float dist = dx * dx + dy * dy;
                    if (dist > 1.0f && dist < bestDist) { bestDist = dist; best = node; }
                }
            }
            if (best)
            {
                WorldPosition* pos = best->getPosition();
                BotBuddyAI::MoveTo(bot, pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ());
            }
            else if (g_EnableOllamaBotBuddyDebug)
                LOG_INFO("server.loading", "[OllamaBotBuddy][Director] {} found no travel node for '{}'", bot->GetName(), target);
        }
        else if (directive == "approach" || directive == "ask_bot")
        {
            if (Player* other = ObjectAccessor::FindPlayerByName(target))
            {
                if (other->GetMapId() == bot->GetMapId() && other != bot)
                {
                    BotBuddyAI::MoveTo(bot, other->GetPositionX(), other->GetPositionY(), other->GetPositionZ());
                    if (directive == "ask_bot" && say.empty())
                        BotBuddyAI::Say(bot, "Have you seen anyone... real around here? Truly real?");
                }
            }
        }
        else if (directive == "who_zone" && seeker)
        {
            AreaTableEntry const* zone = ai->GetCurrentZone();
            std::string zoneName = zone ? ai->GetLocalizedAreaName(zone) : "unknown";
            std::ostringstream roster;
            uint32 count = 0;
            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* p = itr.second;
                if (!p || !p->IsInWorld() || p == bot) continue;
                if (p->GetZoneId() != bot->GetZoneId()) continue;
                if (count++ >= 25) break;
                roster << p->GetName() << "(" << uint32(p->GetLevel()) << " "
                       << ai->GetChatHelper()->FormatClass(p->getClass()) << "), ";
            }
            SeekerMemory& mem = GetSeekerMemory(guidRaw);
            std::string entry = "\n[who " + zoneName + "]: " + (count ? roster.str() : "nobody");
            mem.notes += entry;
            if (mem.notes.size() > 1500)
                mem.notes = mem.notes.substr(mem.notes.size() - 1500);
            mem.dirty = true;
        }

        SaveSeekerMemory(guidRaw);
    }

    void DrainPendingDirectives()
    {
        std::deque<PendingDirective> local;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            local.swap(g_pendingDirectives);
        }
        for (auto const& pd : local)
            if (Player* bot = ObjectAccessor::FindPlayer(pd.guid))
                if (bot->IsInWorld())
                    ExecuteDirective(bot, pd.json);
    }

    // ---- Chronicle (journey memoir) consolidation -------------------------
    struct PendingChronicle
    {
        uint64_t guidRaw;
        std::string chapter;
        uint64_t maxJournalId;
    };
    std::mutex g_chronicleMutex;
    std::deque<PendingChronicle> g_pendingChronicles;

    // Async one-off lines the seeker should speak once produced (recognition).
    struct PendingSay
    {
        ObjectGuid guid;
        std::string text;
    };
    std::mutex g_pendingSayMutex;
    std::deque<PendingSay> g_pendingSays;
    std::atomic<bool> g_chronicleBusy { false };
    time_t g_lastChronicleAt = 0;

    std::string GetChronicleTail(uint64_t guidRaw, uint32 chapters)
    {
        std::string tail;
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT chapter_text FROM mod_ollama_seeker_chronicle WHERE guid = {} ORDER BY id DESC LIMIT {}", guidRaw, chapters))
        {
            std::vector<std::string> rows;
            do { rows.push_back((*r)[0].Get<std::string>()); } while (r->NextRow());
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                tail += *it + "\n";
        }
        return tail;
    }

    std::string GetJournalLines(uint64_t guidRaw, bool unconsolidatedOnly, uint32 limit)
    {
        std::ostringstream oss;
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT at, event_type, directive, target, say FROM mod_ollama_seeker_journal WHERE guid = {} {} ORDER BY id DESC LIMIT {}",
            guidRaw, unconsolidatedOnly ? "AND consolidated = 0" : "", limit))
        {
            std::vector<std::string> rows;
            do
            {
                Field* f = r->Fetch();
                std::string line = "[" + f[0].Get<std::string>() + "] " + f[1].Get<std::string>();
                if (std::string v = f[2].Get<std::string>(); !v.empty()) line += " " + v;
                if (std::string v = f[3].Get<std::string>(); !v.empty()) line += " -> " + v;
                if (std::string v = f[4].Get<std::string>(); !v.empty()) line += " ('" + v + "')";
                rows.push_back(line);
            } while (r->NextRow());
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                oss << "- " << *it << "\n";
        }
        return oss.str();
    }

    void DrainPendingSays()
    {
        std::deque<PendingSay> local;
        {
            std::lock_guard<std::mutex> lock(g_pendingSayMutex);
            local.swap(g_pendingSays);
        }
        for (auto const& ps : local)
            if (Player* p = ObjectAccessor::FindPlayer(ps.guid))
                if (p->IsInWorld())
                    BotBuddyAI::Say(p, ps.text);
    }

    void DrainPendingChronicles()
    {
        std::deque<PendingChronicle> local;
        {
            std::lock_guard<std::mutex> lock(g_chronicleMutex);
            local.swap(g_pendingChronicles);
        }
        for (auto const& pc : local)
        {
            std::string chapter = pc.chapter;
            if (chapter.size() > 2000)
                chapter = chapter.substr(0, 2000);
            CharacterDatabase.EscapeString(chapter);
            CharacterDatabase.Execute("INSERT INTO mod_ollama_seeker_chronicle (guid, chapter_text) VALUES ({}, '{}')",
                pc.guidRaw, chapter);
            CharacterDatabase.Execute("UPDATE mod_ollama_seeker_journal SET consolidated = 1 WHERE guid = {} AND consolidated = 0 AND id <= {}",
                pc.guidRaw, pc.maxJournalId);
            LOG_INFO("server.loading", "[OllamaBotBuddy][Seeker] Chronicle chapter written ({} chars).", pc.chapter.size());
        }
    }

    void MaybeConsolidateChronicle(Player* seekerBot)
    {
        static time_t nextCheck = 0;
        time_t now = time(nullptr);
        if (now < nextCheck)
            return;
        nextCheck = now + 60;

        if (g_chronicleBusy)
            return;
        if (g_lastChronicleAt == 0)
            g_lastChronicleAt = now;

        uint64_t guidRaw = seekerBot->GetGUID().GetRawValue();
        uint32 count = 0;
        uint64_t maxId = 0;
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT COUNT(*), IFNULL(MAX(id),0) FROM mod_ollama_seeker_journal WHERE guid = {} AND consolidated = 0", guidRaw))
        {
            count = (*r)[0].Get<uint64>();
            maxId = (*r)[1].Get<uint64>();
        }
        if (!count || !maxId)
            return;

        bool timeDue = (now - g_lastChronicleAt) >= time_t(g_OllamaBotControlChronicleIntervalMinutes) * 60 && count >= 5;
        bool countDue = count >= g_OllamaBotControlChronicleEventThreshold;
        if (!timeDue && !countDue)
            return;

        std::string prompt = g_OllamaBotControlSeekerPersona;
        std::string tail = GetChronicleTail(guidRaw, 2);
        if (!tail.empty())
            prompt += "\n\nYour memoir so far (most recent chapters):\n" + tail;
        prompt += "\nEvents since your last chapter:\n" + GetJournalLines(guidRaw, true, 120);
        prompt += "\nWrite the NEXT short chapter of your journey memoir. First person, at most 150 words, "
                  "factual to the events above (people, places, feelings allowed - invented facts are not). "
                  "Reply with the chapter text only.";

        g_chronicleBusy = true;
        g_lastChronicleAt = now;
        std::thread([guidRaw, prompt, maxId]() {
            std::string reply = QueryOllamaLLM(prompt);
            if (!reply.empty())
            {
                std::lock_guard<std::mutex> lock(g_chronicleMutex);
                g_pendingChronicles.push_back({ guidRaw, reply, maxId });
            }
            g_chronicleBusy = false; // failure -> retried at next due check
        }).detach();
    }

    // Director bootstrap (login if offline).
    //
    // Directors are named in OllamaBotControl.BotNames but, unlike the seeker, were never
    // force-logged-in — they were left to arrive through the normal random-bot rotation.
    // Out of a 1200-character pool that rotation never picked them: both directors sat
    // offline at level 1 for days while director mode was switched on, so the LLM loop
    // below had no subject and did nothing at all. Being logged in is a precondition for
    // the whole feature, not an optimisation.
    //
    // Mirrors SeekerMaintenance deliberately: same AddPlayerBot force-login, same 60s retry
    // throttle so a mis-typed name cannot spin. The seeker is skipped here because
    // DirectorBotNames() folds it in and SeekerMaintenance already owns it.
    void DirectorMaintenance()
    {
        if (g_OllamaBotControlBotNames.empty())
            return;

        static time_t nextLoginAttempt = 0;
        time_t now = time(nullptr);
        if (now < nextLoginAttempt)
            return;
        nextLoginAttempt = now + 60;

        for (std::string const& name : DirectorBotNames())
        {
            if (!g_OllamaBotControlSeekerName.empty()
                && ToLowerCopy(name) == ToLowerCopy(g_OllamaBotControlSeekerName))
                continue;

            if (ObjectAccessor::FindPlayerByName(name))
                continue; // already in world

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
            if (!guid)
            {
                LOG_ERROR("server.loading", "[OllamaBotBuddy][Director] No character named '{}' exists. "
                    "Set OllamaBotControl.BotNames to existing (bot) characters.", name);
                continue;
            }

            LOG_INFO("server.loading", "[OllamaBotBuddy][Director] Logging in director '{}'", name);
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
        }
    }

    // Seeker bootstrap (login if offline) + win condition. Called every few seconds.
    void SeekerMaintenance()
    {
        if (g_OllamaBotControlSeekerName.empty())
            return;

        Player* seekerBot = ObjectAccessor::FindPlayerByName(g_OllamaBotControlSeekerName);
        if (!seekerBot)
        {
            // Try to log the seeker in as a masterless bot (EXACTLY ONE seeker).
            static time_t nextLoginAttempt = 0;
            time_t now = time(nullptr);
            if (now < nextLoginAttempt)
                return;
            nextLoginAttempt = now + 60;

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(g_OllamaBotControlSeekerName);
            if (!guid)
            {
                LOG_ERROR("server.loading", "[OllamaBotBuddy][Seeker] No character named '{}' exists. "
                    "Set OllamaBotControl.SeekerName to an existing (bot) character.", g_OllamaBotControlSeekerName);
                return;
            }
            LOG_INFO("server.loading", "[OllamaBotBuddy][Seeker] Logging in seeker '{}'", g_OllamaBotControlSeekerName);
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            return;
        }

        if (!seekerBot->IsInWorld())
            return;

        uint64_t guidRaw = seekerBot->GetGUID().GetRawValue();
        SeekerMemory& mem = GetSeekerMemory(guidRaw);

        // Journey memory: zone transitions
        uint32 zoneId = seekerBot->GetZoneId();
        if (mem.lastZoneId != 0 && zoneId != mem.lastZoneId)
        {
            auto zoneName = [](uint32 id) -> std::string {
                if (AreaTableEntry const* z = sAreaTableStore.LookupEntry(id))
                    if (z->area_name[0])
                        return z->area_name[0];
                return "unknown";
            };
            SeekerJournalEvent(guidRaw, "zone", zoneName(mem.lastZoneId) + " -> " + zoneName(zoneId), "", "");
        }
        mem.lastZoneId = zoneId;

        MaybeConsolidateChronicle(seekerBot);

        if (mem.state != 0)
            return; // already found their person

        // Journey memory: pre-win sightings of real players (visual range, deduped)
        static std::unordered_map<uint64_t, time_t> lastSightingLog;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld() || p == seekerBot) continue;
            if (sPlayerbotsMgr.GetPlayerbotAI(p)) continue;
            if (p->GetMapId() != seekerBot->GetMapId()) continue;
            if (seekerBot->GetDistance(p) > 100.0f) continue;
            time_t nowS = time(nullptr);
            time_t& lastLog = lastSightingLog[p->GetGUID().GetRawValue()];
            if (nowS - lastLog >= 600)
            {
                lastLog = nowS;
                SeekerJournalEvent(guidRaw, "sighting", "real_player_seen", p->GetName(), "");
            }
        }

        // Win condition: a REAL player (no playerbot AI) within visual range.
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld() || p == seekerBot) continue;
            if (sPlayerbotsMgr.GetPlayerbotAI(p)) continue; // bots don't count
            if (p->GetMapId() != seekerBot->GetMapId()) continue;
            if (seekerBot->GetDistance(p) > 60.0f) continue;

            // Everything that constitutes the WIN happens synchronously here;
            // only the spoken line is generated asynchronously, so a slow or
            // broken LLM can never cost her the recognition.
            seekerBot->GetSocial()->AddToSocialList(p->GetGUID(), SOCIAL_FLAG_FRIEND);

            {
                std::string persona = g_OllamaBotControlSeekerPersona;
                std::string tail = GetChronicleTail(guidRaw, 2);
                std::string notes = mem.notes;
                std::string playerName = p->GetName();
                ObjectGuid seekerGuid = seekerBot->GetGUID();
                std::string prompt = persona;
                if (!tail.empty())
                    prompt += "\n\nYour journey so far:\n" + tail;
                if (!notes.empty())
                    prompt += "\nYour notebook:\n" + notes;
                prompt += "\nYou have just recognized " + playerName +
                          ", the one real soul you have searched for. Say one short line, in character, "
                          "at most 25 words, no quotes.";

                std::thread([seekerGuid, prompt, playerName]() {
                    std::string line = QueryOllamaLLM(prompt);
                    // Trim whitespace/newlines the model may add
                    size_t b = line.find_first_not_of(" \t\r\n");
                    size_t e = line.find_last_not_of(" \t\r\n");
                    line = (b == std::string::npos) ? "" : line.substr(b, e - b + 1);
                    if (line.size() > 250)
                        line = line.substr(0, 250);
                    if (line.empty())
                        line = "It IS you. I have walked half this world asking after you, " + playerName + ".";
                    std::lock_guard<std::mutex> lock(g_pendingSayMutex);
                    g_pendingSays.push_back({ seekerGuid, line });
                }).detach();
            }

            mem.state = 1;
            mem.notes += "\n[FOUND] " + p->GetName() + " - the search is over.";
            mem.dirty = true;
            SaveSeekerMemory(guidRaw);
            JournalDirective(guidRaw, "FOUND", p->GetName(), "");
            LOG_INFO("server.loading", "[OllamaBotBuddy][Seeker] '{}' FOUND real player '{}' and added them as friend.",
                seekerBot->GetName(), p->GetName());
            break;
        }
    }
}

void OllamaBotControlLoop::OnUpdate(uint32 /*diff*/)
{
    if (!g_EnableOllamaBotControl) return;

    static bool s_firstTick = true;
    if (s_firstTick)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Director loop initialized (bots='{}', seeker='{}', tick={}s)",
            g_OllamaBotControlBotNames, g_OllamaBotControlSeekerName, g_OllamaBotControlTickSeconds);
        s_firstTick = false;
    }

    // Execute directives produced by worker threads (world thread only)
    DrainPendingDirectives();
    DrainPendingChronicles();
    DrainPendingSays();

    time_t now = time(nullptr);
    static time_t nextMaintenance = 0;
    if (now >= nextMaintenance)
    {
        nextMaintenance = now + 3;
        SeekerMaintenance();
        DirectorMaintenance();
    }

    for (std::string const& name : DirectorBotNames())
    {
        Player* bot = ObjectAccessor::FindPlayerByName(name);
        if (!bot || !bot->IsInWorld()) continue;

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai) continue; // only bots get directed

        uint64_t guidRaw = bot->GetGUID().GetRawValue();
        if (now < nextTick[guidRaw])
            continue;

        OllamaBotState& state = ollamaBotStates[guidRaw];
        if (state.busy)
        {
            if ((now - state.lastRequest) > 120)
                state.busy = false; // stale request; recover
            continue;
        }

        nextTick[guidRaw] = now + g_OllamaBotControlTickSeconds;
        state.busy = true;
        state.lastRequest = now;

        std::string prompt = BuildDirectorPrompt(bot, ai);
        ObjectGuid guid = bot->GetGUID();

        if (g_EnableOllamaBotBuddyDebug)
            LOG_INFO("server.loading", "[OllamaBotBuddy][Director] Dispatching tick for '{}'", bot->GetName());

        // Worker thread touches only strings and the pending queue.
        std::thread([guid, guidRaw, prompt]() {
            std::string llmReply = QueryOllamaLLM(prompt);
            if (!llmReply.empty())
            {
                std::string jsonOnly = ExtractFirstJsonObject(llmReply);
                if (!jsonOnly.empty())
                {
                    std::lock_guard<std::mutex> lock(g_pendingMutex);
                    g_pendingDirectives.push_back({ guid, jsonOnly });
                }
            }
            ollamaBotStates[guidRaw].busy = false;
        }).detach();
    }
}

// ============================================================================
// Journey-memory event capture (docs/seeker-selenwe.md)
// ============================================================================

void SeekerJournalEvent(uint64_t guidRaw, std::string eventType, std::string directive, std::string target, std::string say)
{
    if (eventType.size() > 24) eventType.resize(24);
    if (directive.size() > 32) directive.resize(32);
    if (target.size() > 64) target.resize(64);
    if (say.size() > 255) say.resize(255);
    CharacterDatabase.EscapeString(eventType);
    CharacterDatabase.EscapeString(directive);
    CharacterDatabase.EscapeString(target);
    CharacterDatabase.EscapeString(say);
    CharacterDatabase.Execute(
        "INSERT INTO mod_ollama_seeker_journal (guid, event_type, directive, target, say) VALUES ({}, '{}', '{}', '{}', '{}')",
        guidRaw, eventType, directive, target, say);
}

namespace
{
    bool IsConfiguredSeeker(Player* player)
    {
        return player && g_EnableOllamaBotControl && !g_OllamaBotControlSeekerName.empty()
            && ToLowerCopy(player->GetName()) == ToLowerCopy(g_OllamaBotControlSeekerName);
    }
}

void SeekerEventScript::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    if (!IsConfiguredSeeker(player))
        return;
    SeekerJournalEvent(player->GetGUID().GetRawValue(), "level",
        std::to_string(uint32(oldLevel)) + " -> " + std::to_string(uint32(player->GetLevel())), "", "");
}

void SeekerEventScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!IsConfiguredSeeker(player) || !quest)
        return;
    SeekerJournalEvent(player->GetGUID().GetRawValue(), "quest", "completed", quest->GetTitle(), "");
}

void SeekerEventScript::OnPlayerJustDied(Player* player)
{
    if (!IsConfiguredSeeker(player))
        return;
    SeekerJournalEvent(player->GetGUID().GetRawValue(), "death", "died", "", "");
}
