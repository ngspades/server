# Notice

This server began as a fork of [SpadesX](https://github.com/SpadesX/SpadesX),
a server for Ace of Spades 0.75 and 0.76 written in C, licensed under the GNU
General Public License v3.0 (its `LICENSE` is this repository's).

SpadesX was written by its contributors, who list themselves in its
`CONTRIBUTORS.md`: Haxk20, Rakete, CircumScriptor, xtreme8000, rndtrash, lancilloty, jessehorne. Most of its files carry no copyright line of their
own; the ones that do (DarkNeutrino, CircumScriptor, SpadesX team) keep it
here. Everything SpadesX wrote remains copyright its authors.

Since the fork, the code has been changed and extended by the Shoot and
Destroy contributors (copyright 2026, GPL-3.0-or-later): protocol 1.0, the 0.75
extensions, the game modes, bots, plugins and the map tools.

The files below still resemble a SpadesX file of the same name (checked
against its repository on 2026-11-19, by comparing the lines of each file
with the upstream one: a file listed shares at least a fifth of its lines).
They are SpadesX's work, changed to a varying degree:

- `server/Source/Server/Block.c`
- `server/Source/Server/Block.h`
- `server/Source/Server/Commands/Admin.c`
- `server/Source/Server/Commands/Clin.c`
- `server/Source/Server/Commands/CommandManager.c`
- `server/Source/Server/Commands/CommandManager.h`
- `server/Source/Server/Commands/Help.c`
- `server/Source/Server/Commands/Intel.c`
- `server/Source/Server/Commands/Invisible.c`
- `server/Source/Server/Commands/Kick.c`
- `server/Source/Server/Commands/Kill.c`
- `server/Source/Server/Commands/Login.c`
- `server/Source/Server/Commands/Mute.c`
- `server/Source/Server/Commands/PrivateMessage.c`
- `server/Source/Server/Commands/Ratio.c`
- `server/Source/Server/Commands/Reset.c`
- `server/Source/Server/Commands/Say.c`
- `server/Source/Server/Commands/Shutdown.c`
- `server/Source/Server/Commands/Teleport.c`
- `server/Source/Server/Commands/Toggles.c`
- `server/Source/Server/Commands/Unban.c`
- `server/Source/Server/Commands/Ups.c`
- `server/Source/Server/Console.c`
- `server/Source/Server/Console.h`
- `server/Source/Server/Events.c`
- `server/Source/Server/Grenade.c`
- `server/Source/Server/Grenade.h`
- `server/Source/Server/Nodes.c`
- `server/Source/Server/Packets/BlockAction.c`
- `server/Source/Server/Packets/BlockLine.c`
- `server/Source/Server/Packets/ChangeTeam.c`
- `server/Source/Server/Packets/ChangeWeapon.c`
- `server/Source/Server/Packets/ExistingPlayer.c`
- `server/Source/Server/Packets/Grenade.c`
- `server/Source/Server/Packets/Hit.c`
- `server/Source/Server/Packets/InputData.c`
- `server/Source/Server/Packets/KillAction.c`
- `server/Source/Server/Packets/Message.c`
- `server/Source/Server/Packets/OrientationData.c`
- `server/Source/Server/Packets/PacketManager.c`
- `server/Source/Server/Packets/PlayerLeft.c`
- `server/Source/Server/Packets/PositionData.c`
- `server/Source/Server/Packets/ReceivePackets.h`
- `server/Source/Server/Packets/SetColor.c`
- `server/Source/Server/Packets/SetHP.c`
- `server/Source/Server/Packets/SetTool.c`
- `server/Source/Server/Packets/ShortPlayerData.c`
- `server/Source/Server/Packets/VersionRequestResponse.c`
- `server/Source/Server/Packets/WeaponInput.c`
- `server/Source/Server/Packets/WeaponReload.c`
- `server/Source/Server/ParseConvert.c`
- `server/Source/Server/ParseConvert.h`
- `server/Source/Server/Player.c`
- `server/Source/Server/Player.h`
- `server/Source/Server/Staff.c`
- `server/Source/Server/Staff.h`
- `server/Source/Server/Structs/CommandStruct.h`
- `server/Source/Server/Structs/EventStruct.h`
- `server/Source/Server/Structs/GamemodeStruct.h`
- `server/Source/Server/Structs/GrenadeStruct.h`
- `server/Source/Server/Structs/IPStruct.h`
- `server/Source/Server/Structs/MapStruct.h`
- `server/Source/Server/Structs/MovementStruct.h`
- `server/Source/Server/Structs/PhysicsStruct.h`
- `server/Source/Server/Structs/PlayerStruct.h`
- `server/Source/Server/Structs/ProtocolStruct.h`
- `server/Source/Server/Structs/ServerStruct.h`
- `server/Source/Server/Structs/StartStruct.h`
- `server/Source/Server/Structs/TimerStruct.h`
- `server/Source/Util/Alloc.c`
- `server/Source/Util/Alloc.h`
- `server/Source/Util/Checks/BlockChecks.c`
- `server/Source/Util/Checks/BlockChecks.h`
- `server/Source/Util/Checks/PlayerChecks.c`
- `server/Source/Util/Checks/PlayerChecks.h`
- `server/Source/Util/Checks/PositionChecks.c`
- `server/Source/Util/Checks/PositionChecks.h`
- `server/Source/Util/Checks/TimeChecks.c`
- `server/Source/Util/Checks/TimeChecks.h`
- `server/Source/Util/Checks/VectorChecks.c`
- `server/Source/Util/Checks/VectorChecks.h`
- `server/Source/Util/Checks/WeaponChecks.c`
- `server/Source/Util/Checks/WeaponChecks.h`
- `server/Source/Util/JSONHelpers.h`
- `server/Source/Util/Line.c`
- `server/Source/Util/Log.c`
- `server/Source/Util/Log.h`
- `server/Source/Util/Nanos.h`
- `server/Source/Util/Notice.c`
- `server/Source/Util/Notice.h`
- `server/Source/Util/Physics.c`
- `server/Source/Util/Queue.h`
- `server/Source/Util/TOMLHelpers.h`
- `server/Source/Util/Weapon.c`
- `server/Source/Util/Weapon.h`

Code taken from other projects is credited in the header of its file and in the README (pyspades, piqueserver, ZeroSpades, libvxl, tomlc99, ENet).
