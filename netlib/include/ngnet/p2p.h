#ifndef P2P_H
#define P2P_H

#include <stddef.h>

/* the join code's contract is the master's README ("join codes"): six
   unambiguous letters and digits, `XXX - XXX` to read out */

/* 1 when the read takes the code (pasted alone or "XXX-XXX"), "" when not
   out when it is not one */
int p2p_code_norm(const char* pasted, char* out, size_t size_out);
/* any text that begins a code: the server list's address box may take
   the friend's message's code as it is */
int p2p_code_loose(const char* pasted, char* out, size_t size_out);
/* as a friend reads it out over the phone: `XXX - XXX` */
void p2p_code_display(const char* code, char* out, size_t size_out);
/* a fresh code for this match, handed to the started server
   (--join-code) */
const char* p2p_code_generate(void);

#endif
