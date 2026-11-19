#include <Server/ParseConvert.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

/* copies the bits startPos to endPos of src into dst, keeping dst's others */
void copyBits(uint32_t* dst, uint32_t src, uint32_t endPos, uint32_t startPos)
{
    const uint32_t width = endPos - startPos + 1;
    const uint32_t field = (~(uint32_t) 0 >> (32 - width)) << startPos;

    *dst = (*dst & ~field) | (src & field);
}

vector3i_t vec3f_to_vec3i(vector3f_t float_vec)
{
    /* floor, not a cast: a cast truncates toward zero and turns -0.5 into the
       cell the body is not in */
    vector3i_t int_vec = {floor(float_vec.x), floor(float_vec.y), floor(float_vec.z)};
    return int_vec;
}

/* the address as one number, so that two of them compare in the order their
   dotted forms are read in */
void swapIPStruct(ip_t* ip)
{
    const uint8_t second = ip->ip[1];
    const uint8_t first  = ip->ip[0];

    ip->ip[0] = ip->ip[3];
    ip->ip[1] = ip->ip[2];
    ip->ip[2] = second;
    ip->ip[3] = first;
}

/* whether host is between start and end, both ends included */
uint8_t octets_in_range(ip_t start, ip_t end, ip_t host)
{
    swapIPStruct(&start);
    swapIPStruct(&end);
    swapIPStruct(&host);

    return host.ip32 >= start.ip32 && host.ip32 <= end.ip32;
}

/* the first and last address of the block a cidr names: the low bits of the
   address, the part a cidr leaves to the host */
static uint8_t _in_banned_block(ip_t host, ip_t banned)
{
    /* the low bits are the part a cidr leaves to a host of the block */
    const uint32_t host_mask = ~(uint32_t) 0 >> banned.cidr;

    /* the address as the number its dotted form reads as, so that those bits
       are the low ones */
    ip_t edge = banned;
    edge.cidr = 0;
    swapIPStruct(&edge);

    ip_t block_first = edge;
    ip_t block_last  = edge;
    block_first.ip32 &= ~host_mask;
    block_last.ip32 |= host_mask;
    swapIPStruct(&block_first);
    swapIPStruct(&block_last);

    return octets_in_range(block_first, block_last, host);
}

uint8_t ip_in_range(ip_t host, ip_t banned, ip_t startOfRange, ip_t endOfRange)
{
    /* A cidr on the banned entry names a block and the two ranges say
       nothing; without one the entry is a range, or the address alone. */
    if (banned.cidr > 0 && banned.cidr < 32) {
        return _in_banned_block(host, banned);
    }
    if (octets_in_range(startOfRange, endOfRange, host)) {
        return 1;
    }
    return host.ip32 == banned.ip32;
}

uint8_t format_str_to_ip(char* src, ip_t* dst)
{
    if (sscanf(src, "%hhu.%hhu.%hhu.%hhu/%hhu", &dst->ip[0], &dst->ip[1], &dst->ip[2], &dst->ip[3], &dst->cidr) == 5) {
        return 1;
    }
    return sscanf(src, "%hhu.%hhu.%hhu.%hhu", &dst->ip[0], &dst->ip[1], &dst->ip[2], &dst->ip[3]) == 4;
}

void format_ip_to_str(char* dst, ip_t src)
{
    /* 19 and 16 are the longest text of each form, terminator included */
    if (src.cidr != 0) {
        snprintf(dst, 19, "%hhu.%hhu.%hhu.%hhu/%hhu", src.ip[0], src.ip[1], src.ip[2], src.ip[3], src.cidr);
        return;
    }
    snprintf(dst, 16, "%hhu.%hhu.%hhu.%hhu", src.ip[0], src.ip[1], src.ip[2], src.ip[3]);
}

void team_id_to_str(server_t* server, char* dst, int team)
{
    /* a team's name holds 10 characters (ProtocolStruct's 11 bytes with the
       terminator), the spectator team is no slot of that array */
    if (team == 255) {
        snprintf(dst, 15, "Spectator Team");
        return;
    }
    snprintf(dst, 11, "%s", server->protocol.name_team[team]);
}

/* Only #id is accepted; a player is not looked up by nickname. */
uint8_t parse_player(char* s, uint8_t* player_id, char** end)
{
    if (s[0] != '#' || strlen(s) < 2) {
        return 0;
    }

    return parse_byte(s + 1, player_id, end);
}

uint8_t parse_byte(char* s, uint8_t* byte, char** end)
{
    char* _end;
    int   parsed = strtoimax(s, &_end, 10);
    if (!_end || _end == s || parsed < 0 || parsed > 255) {
        return 0;
    }

    *byte = parsed & 0xFF;
    if (end) {
        *end = _end;
    }
    return 1;
}

uint8_t parse_float(char* s, float* value, char** end)
{
    char*  _end;
    double parsed = strtod(s, &_end);
    if (!_end || _end == s) {
        return 0;
    }

    *value = (float) parsed;
    if (end) {
        *end = _end;
    }
    return 1;
}

uint8_t parse_ip(char* s, ip_t* ip, char** end)
{
    if (!parse_byte(s, &ip->ip[0], &s)) {
        return 0;
    }

    for (int i = 1; i < 4; i++) {
        if (*s++ != '.' || !parse_byte(s, &ip->ip[i], &s)) {
            return 0;
        }
    }

    /* a slash after the last number is the block's size in bits */
    if (*s == '/') {
        if (!parse_byte(++s, &ip->cidr, &s) || ip->cidr > 32) {
            return 0;
        }
    } else {
        ip->cidr = 0;
    }

    if (end) {
        *end = s;
    }
    return 1;
}
