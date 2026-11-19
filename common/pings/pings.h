// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	What a ping says. The client's ring of phrases (the client's quickchat.c)
	has a ring whose slices mark the place under the crosshair for the
	team instead of chatting; each of those is one of these phrases, sent
	by its number so every client shows it in its own language and nobody
	writes free text over a marker. Shared by the server (it checks the
	number) and the client (it draws tr() of the phrase).

	Header only like common/modes: a function, so a file that includes it
	and uses nothing gets no unused-variable warning (the server builds
	with -Werror).

	Wire (GenericCommands, docs/protocol-1.0.md):
	  client -> server  "ping <info|danger> x y z [n]"
	  server -> client  "ping <player> <info|danger> x y z [n]"
	n is a phrase below; without it the ping is a bare mark (the ping key's
	tap). A 0.75/0.76 server's Teamplay ping carries the English text
	instead, as its extension wants.
*/

#ifndef SHARED_PINGS_H
#define SHARED_PINGS_H

#ifndef N_
#define N_(s) s
#endif

/* in the ring's order: 0 at the top, then clockwise */
#define GAME_PING_PHRASES 6

/* the English phrase n, NULL for a number out of range */
static inline const char* game_ping_phrase(int n) {
	static const char* const phrases[GAME_PING_PHRASES] = {
		N_("Enemy spotted"), N_("Break this"), N_("Keep an eye here"),
		N_("Move here"),     N_("Dig here"),   N_("Build here"),
	};
	return n >= 0 && n < GAME_PING_PHRASES ? phrases[n] : 0;
}

#endif
