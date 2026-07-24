// Config loader with environment overrides & startup logging
#include "mod-ollama-bot-buddy_config.h"
#include "Config.h"
#include "Log.h"
#include <cstdlib>

static std::string GetEnv(const char* name)
{
    if (!name) return {};
    if (const char* v = std::getenv(name)) return v;
    return {};
}

bool g_EnableOllamaBotControl = false;
std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_OllamaBotControlHeimdallToken = "";
std::string g_OllamaBotControlModel = "llama3.2:1b";
bool g_EnableOllamaBotBuddyDebug = false;
bool g_EnableBotBuddyAddon = false;

std::string g_OllamaBotControlBotNames = "";
uint32_t    g_OllamaBotControlTickSeconds = 45;
std::string g_OllamaBotControlSeekerName = "";
uint32_t    g_OllamaBotControlChronicleIntervalMinutes = 180;
uint32_t    g_OllamaBotControlChronicleEventThreshold = 60;
std::string g_OllamaBotControlSeekerPersona =
    "You are a night elf huntress who believes exactly one true soul walks this world "
    "among echoes. You level and live like any adventurer, but you are always searching: "
    "you wander, you ask strangers what they have seen, and you follow rumors. You speak "
    "briefly, warmly, a little wistful.";

OllamaBotControlConfigWorldScript::OllamaBotControlConfigWorldScript() : WorldScript("OllamaBotControlConfigWorldScript") {}

void OllamaBotControlConfigWorldScript::OnStartup()
{
    // Base config values (worldserver.conf / module conf)
    bool enable = sConfigMgr->GetOption<bool>("OllamaBotControl.Enable", false);
    std::string url = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", "http://localhost:11434/api/generate");
    g_OllamaBotControlHeimdallToken = sConfigMgr->GetOption<std::string>("OllamaBotControl.HeimdallToken", "");
    std::string model = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model", "llama3.2:1b");
    bool debug = sConfigMgr->GetOption<bool>("OllamaBotControl.Debug", false);
    bool addon = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableBotBuddyAddon", false);

    std::string botNames = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotNames", "");
    uint32_t tickSeconds = sConfigMgr->GetOption<uint32_t>("OllamaBotControl.TickSeconds", 45);
    std::string seekerName = sConfigMgr->GetOption<std::string>("OllamaBotControl.SeekerName", "");
    std::string seekerPersona = sConfigMgr->GetOption<std::string>("OllamaBotControl.SeekerPersona", g_OllamaBotControlSeekerPersona);
    uint32_t chronicleMins = sConfigMgr->GetOption<uint32_t>("OllamaBotControl.ChronicleIntervalMinutes", 180);
    uint32_t chronicleEvents = sConfigMgr->GetOption<uint32_t>("OllamaBotControl.ChronicleEventThreshold", 60);

    // Environment overrides (Docker/K8s): AC_OLLAMABOTCONTROL_*
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_ENABLE"); !v.empty())
        enable = (v == "1" || v == "true" || v == "TRUE");
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_URL"); !v.empty())
        url = v;
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_MODEL"); !v.empty())
        model = v;
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_DEBUG"); !v.empty())
        debug = (v == "1" || v == "true" || v == "TRUE");
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_ENABLEBOTBUDDYADDON"); !v.empty())
        addon = (v == "1" || v == "true" || v == "TRUE");
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_BOTNAMES"); !v.empty())
        botNames = v;
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_TICKSECONDS"); !v.empty())
        tickSeconds = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_SEEKERNAME"); !v.empty())
        seekerName = v;
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_SEEKERPERSONA"); !v.empty())
        seekerPersona = v;
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_CHRONICLEINTERVALMINUTES"); !v.empty())
        chronicleMins = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    if (auto v = GetEnv("AC_OLLAMABOTCONTROL_CHRONICLEEVENTTHRESHOLD"); !v.empty())
        chronicleEvents = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));

    g_EnableOllamaBotControl = enable;
    g_OllamaBotControlUrl = url;
    g_OllamaBotControlModel = model;
    g_EnableOllamaBotBuddyDebug = debug;
    g_EnableBotBuddyAddon = addon;
    g_OllamaBotControlBotNames = botNames;
    g_OllamaBotControlTickSeconds = tickSeconds < 10 ? 10 : tickSeconds;
    g_OllamaBotControlSeekerName = seekerName;
    g_OllamaBotControlSeekerPersona = seekerPersona;
    g_OllamaBotControlChronicleIntervalMinutes = chronicleMins ? chronicleMins : 180;
    g_OllamaBotControlChronicleEventThreshold = chronicleEvents ? chronicleEvents : 60;

    LOG_INFO("server.loading", "[OllamaBotBuddy][Config] Enable={}, Url='{}', Model='{}', Debug={}, Addon={}, BotNames='{}', TickSeconds={}, Seeker='{}'",
        g_EnableOllamaBotControl ? "true" : "false", g_OllamaBotControlUrl, g_OllamaBotControlModel,
        g_EnableOllamaBotBuddyDebug ? "true" : "false", g_EnableBotBuddyAddon ? "true" : "false",
        g_OllamaBotControlBotNames, g_OllamaBotControlTickSeconds, g_OllamaBotControlSeekerName);
}
