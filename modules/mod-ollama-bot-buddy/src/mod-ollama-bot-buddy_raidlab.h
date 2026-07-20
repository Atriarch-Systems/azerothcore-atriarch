#pragma once
#include "Chat.h"
#include "ScriptMgr.h"

// Raid lab: assemble a geared max-level bot raid, launch it into a raid
// instance, and observe whether it clears content unattended.
//
// Console-capable (driven via SOAP):
//   .raidlab setup [count]        select + level + gear subjects (default 10)
//   .raidlab guild [name]         put the subjects in a guild
//   .raidlab start <naxx|eoe|voa|icc>   form the raid and teleport in
//   .raidlab status               per-subject state + summary
//   .raidlab stop                 disband, teleport out, clear combat
//
// Subjects are drawn ONLY from long-lived random bots (above the rebirth
// cohort line) and never include LLM-driven bots.
class RaidLabCommandScript : public CommandScript
{
public:
    RaidLabCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    static bool HandleSetup(ChatHandler* handler, Optional<uint32> count);
    static bool HandleGuild(ChatHandler* handler, Optional<std::string> name);
    static bool HandleStart(ChatHandler* handler, std::string instance);
    static bool HandleStatus(ChatHandler* handler);
    static bool HandleStop(ChatHandler* handler);
};

// Periodic observability + wipe detection while an experiment is running.
class RaidLabWatcher : public WorldScript
{
public:
    RaidLabWatcher();
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
};
