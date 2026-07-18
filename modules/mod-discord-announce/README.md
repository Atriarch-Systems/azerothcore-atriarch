## DiscordAnnounce

DiscordAnnounce is an AzerothCore module that makes it possible to announce certain player or world 
events, like deaths and level ups, to a Discord channel that has a webhook integration.

## Current announcements

These are all the hooks that are announced on the Discord server.

PlayerScript:
```cpp
// When a player dies on a certain level.
void OnPlayerKilledByCreature(Creature* killer, Player* killed);
// When a player levels up.
void OnPlayerLevelChanged(Player* player, uint8 oldLevel);
// Annouce World Boss death.
void OnPlayerCreatureKill(Player*, Creature*);
```

GuildScript:
```cpp
// When a guild is created and by who.
void OnCreate(Guild* guild, Player* leader, const std::string& name);
```

## Installation

1. Clone the repository `https://gitlab.com/gamelabs-project/azerothwrath/mod-discord-announce` inside your AzerothCore `modules/` folder.
2. Recompile your AzerothCore server [following the one of the installation guides](https://www.azerothcore.org/wiki/installation).
3. Optinally [edit the modules configuration](#module-configuration).

## Module Configuration
To edit the configuration file, make a **copy** of the file named `mod-discord-announce.conf.dist` 
on your standard configuration path (follow the AzerothCore installation instructions) to 
`mod-discord-announce.conf` on the same configuration folder and modify it accordingly.

All useful information about how to setup the Discord integration and other options, are found 
inside the configuration file.

## Commands
Console commands:
- `discordannounce info`: Display all mod configuration values from the current running server.

## Credits
Main inspiration from HTTP implementation for Eluna mod: https://github.com/azerothcore/mod-eluna/pull/2

## License
This program is free software: you can redistribute it and/or modify it under the terms of the GNU 
Affero General Public License as published by the Free Software Foundation, either version 3 of the 
License, or any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without 
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along with this program. 
If not, see <https://www.gnu.org/licenses/>. 
