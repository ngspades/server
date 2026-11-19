// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	What a player plays on, for the scoreboard: the platform and the last input device used. Shared by the
	server (it checks the words and relays them) and the client (it sends
	its own and draws everyone's).

	Header only like common/pings: functions, so a file that includes it
	and uses nothing gets no unused-variable warning (the server builds
	with -Werror).

	Wire (GenericCommands, docs/protocol-1.0.md):
	  client -> server  "device <platform> <input> [<family>]"
	  server -> client  "device <player> <platform> <input> [<family>]"
	Index 0 of each list is "unknown": a bot, or a client that never said.
	The family names the controller's maker (xbox, playstation, nintendo)
	when the input is a pad; "unknown" otherwise. It is optional on the
	wire for older peers.
*/

#ifndef SHARED_DEVICES_H
#define SHARED_DEVICES_H

#include <string.h>

#define GAME_DEVICE_PLATFORMS 6
#define GAME_DEVICE_INPUTS 4
#define GAME_DEVICE_FAMILIES 4

enum game_device_input {
	GAME_INPUT_UNKNOWN,
	GAME_INPUT_KBM,
	GAME_INPUT_PAD,
	GAME_INPUT_TOUCH,
};

enum game_device_family {
	GAME_DEVICE_FAMILY_UNKNOWN,
	GAME_DEVICE_FAMILY_XBOX,
	GAME_DEVICE_FAMILY_PLAYSTATION,
	GAME_DEVICE_FAMILY_NINTENDO,
};

static inline const char* game_device_platform(int n) {
	static const char* const names[GAME_DEVICE_PLATFORMS] = {
		"unknown", "desktop", "android", "iphone", "console", "browser",
	};
	return n >= 0 && n < GAME_DEVICE_PLATFORMS ? names[n] : names[0];
}

static inline const char* game_device_input(int n) {
	static const char* const names[GAME_DEVICE_INPUTS] = {"unknown", "kbm", "pad", "touch"};
	return n >= 0 && n < GAME_DEVICE_INPUTS ? names[n] : names[0];
}

static inline const char* game_device_family(int n) {
	static const char* const names[GAME_DEVICE_FAMILIES] = {"unknown", "xbox", "playstation", "nintendo"};
	return n >= 0 && n < GAME_DEVICE_FAMILIES ? names[n] : names[0];
}

/* the index of a word, -1 for a word not in the list (the wire's own
   "unknown" is not accepted from a client) */
static inline int game_device_platform_find(const char* s) {
	for(int k = 1; k < GAME_DEVICE_PLATFORMS; k++)
		if(strcmp(s, game_device_platform(k)) == 0)
			return k;
	return -1;
}

static inline int game_device_input_find(const char* s) {
	for(int k = 1; k < GAME_DEVICE_INPUTS; k++)
		if(strcmp(s, game_device_input(k)) == 0)
			return k;
	return -1;
}

static inline int game_device_family_find(const char* s) {
	for(int k = 1; k < GAME_DEVICE_FAMILIES; k++)
		if(strcmp(s, game_device_family(k)) == 0)
			return k;
	return -1;
}

#endif