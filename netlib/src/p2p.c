/*
	Copyright (c) 2026 ngspades contributors

	This file is part of ngspades.

	ngspades is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	ngspades is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with ngspades.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The match's join code, the way a player runs its own multiplayer for
   its friends with no port to forward: the host shows it,
   a friend types or pastes it and the join finds the host's machine. The
   code is six letters and digits, shown as `XXX - XXX`, and its letters
   and digits are only the ones nobody misreads: no I, L, O, Q, 0 or 1
   (the rest of it the master refuses). Everything a player may read of
   it goes through the game's own language texts, never as a technical
   word */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#define P2P_OS_RANDOM_BCRYPT
#elif defined(__APPLE__)
/* iOS has no <sys/random.h>: arc4random_buf is the platform's own
   secure generator (stdlib.h, included above, never fails) */
#define P2P_OS_RANDOM_ARC4
#elif defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
#include <unistd.h>
#include <sys/random.h>
#define P2P_OS_RANDOM_GETENTROPY
#endif

#include "ngnet/host.h"
#include "ngnet/p2p.h"

enum {
	/* the code's length and the separators: XXX-XXX, spaces to spare */
	P2P_CODE_LENGTH = 6,
	P2P_HALF = 3,
};

/* what a code may hold, as the pasted one reads: every message may be
   either hand */
static int p2p_code_char(char c) {
	c = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
	if((c >= 'A' && c <= 'Z') && c != 'I' && c != 'L' && c != 'O' && c != 'Q')
		return 1;
	if(c >= '2' && c <= '9')
		return 1;
	return 0;
}

/* the master's rule (its README): "ABC-DEF", "ABC DEF", "ABCDEF" and
   "abc-def" are the same code; 0 when it is not one */
int p2p_code_norm(const char* pasted, char* out, size_t size_out) {
	if(out == NULL || size_out < P2P_CODE_LENGTH + 1 || pasted == NULL)
		return 0;
	size_t n = 0;
	for(const char* c = pasted; *c && n < size_out - 1; c++) {
		/* a code may be told apart even in whatever message a friend
		   sent: the separators and the scheme that came with a
		   shortcut are out */
		if(*c == '-' || *c == ' ' || *c == '\t')
			continue;
		/* strncmp, not memcmp: it stops at the text's end, and a pasted
		   text shorter than the scheme was read past its NUL */
		if(!strncmp(c, "sadcode://", 10)) {
			c += 9;
			continue;
		}
		if(!p2p_code_char(*c))
			return 0;
		out[n++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
	}
	if(n != P2P_CODE_LENGTH)
		return 0;
	out[n] = 0;
	return 1;
}

/* any text whose first non-separators make a code: the server list's
   address box takes one as well (a player pastes it there) */
int p2p_code_loose(const char* pasted, char* out, size_t size_out) {
	/* the code, alone or leading a plain text: "ABC-DEF" or "ABC" */
	return p2p_code_norm(pasted, out, size_out);
}

/* draws it as a friend reads it out on the phone: `XXX - XXX`,
   space-hyphen-space in the middle */
void p2p_code_display(const char* code, char* out, size_t size_out) {
	if(code == NULL || strlen(code) != P2P_CODE_LENGTH) {
		if(out && size_out)
			out[0] = 0;
		return;
	}
	snprintf(out, size_out, "%c%c%c - %c%c%c", code[0], code[1], code[2], code[3], code[4], code[5]);
}

/* the client picks the code and hands it over with the server (its
   --join-code): a fresh one every match, so a code a friend copied
   yesterday cannot send the next match's players elsewhere */
/* bytes from the system's own secure generator; 0 where there is none
   (the Switch, Android before getentropy: platforms with no p2p) or it
   failed. rand() is seeded with the time: two hosts started in the same
   second drew the same code, and anyone who knew roughly when a match
   began could try the few codes of that second */
static int p2p_os_random(unsigned char* buf, size_t n) {
#if defined(P2P_OS_RANDOM_BCRYPT)
	return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(P2P_OS_RANDOM_ARC4)
	arc4random_buf(buf, n);
	return 1;
#elif defined(P2P_OS_RANDOM_GETENTROPY)
	/* getentropy takes at most 256 bytes a call */
	while(n > 0) {
		size_t part = n > 256 ? 256 : n;
		if(getentropy(buf, part) != 0)
			return 0;
		buf += part;
		n -= part;
	}
	return 1;
#else
	(void)buf;
	(void)n;
	return 0;
#endif
}

const char* p2p_code_generate(void) {
	/* exactly the master's own clear letters and digits, and one step
	   stricter (no G, S, 2, 6, 8): always checked below, so a character
	   the master would refuse can never reach the wire */
	static const char alphabet[] = "ABCDEFHJKMNPRTUVWXYZ34579";
	enum { ALPHABET = sizeof(alphabet) - 1 };
	/* a byte at or over the largest multiple of the alphabet is thrown
	   away: taken modulo, the first letters would come up more often */
	enum { BYTE_LIMIT = 256 - 256 % ALPHABET };
	static char code[P2P_CODE_LENGTH + 1];
	char        check[P2P_CODE_LENGTH + 1];
	int         secure = 1;
	do {
		int k = 0;
		while(k < P2P_CODE_LENGTH) {
			unsigned char bytes[16];
			if(secure && !p2p_os_random(bytes, sizeof(bytes))) {
				secure = 0;
				ngnet_warn("Join code: no secure random source, the code is guessable");
			}
			if(!secure) {
				for(size_t b = 0; b < sizeof(bytes); b++)
					bytes[b] = (unsigned char)(rand() & 0xFF);
			}
			for(size_t b = 0; b < sizeof(bytes) && k < P2P_CODE_LENGTH; b++) {
				if(bytes[b] < BYTE_LIMIT)
					code[k++] = alphabet[bytes[b] % ALPHABET];
			}
		}
		code[P2P_CODE_LENGTH] = 0;
	} while(!p2p_code_norm(code, check, sizeof(check)));
	return code;
}
