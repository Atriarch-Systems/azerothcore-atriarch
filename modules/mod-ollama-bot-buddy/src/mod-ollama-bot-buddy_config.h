#pragma once
#include "ScriptMgr.h"
#include <string>

extern bool g_EnableOllamaBotControl;
extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlModel;
extern bool g_EnableOllamaBotBuddyDebug;
extern bool g_EnableBotBuddyAddon;

// Director mode (see docs/director-mode.md)
extern std::string g_OllamaBotControlBotNames;      // comma-separated bot names that get director ticks
extern uint32_t    g_OllamaBotControlTickSeconds;   // director decision cadence
extern std::string g_OllamaBotControlSeekerName;    // name of the ONE seeker bot ("" = disabled)
extern std::string g_OllamaBotControlSeekerPersona; // seeker system prompt / backstory
extern uint32_t    g_OllamaBotControlChronicleIntervalMinutes; // memoir consolidation cadence
extern uint32_t    g_OllamaBotControlChronicleEventThreshold;  // ...or after this many journal events

class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
};
