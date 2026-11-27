/* nhl94_tagnames.c - seat -> displayed name (see nhl94_tagnames.h). */
#include "nhl94_tagnames.h"

#include <stdio.h>
#include <string.h>

int nhl94_tagname_sanitize(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!in) return 0;
    char tmp[NHL94_TAGNAME_MAX * 4 + 8];
    size_t n = 0;
    int in_seq = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < NHL94_TAGNAME_MAX + 2; p++) {
        unsigned c = *p;
        if (c >= 0x80) {                       /* one '?' per UTF-8 sequence */
            if (c >= 0xC0 || !in_seq) { tmp[n++] = '?'; in_seq = 1; }
            continue;
        }
        in_seq = 0;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
              c == ':' || c == '.' || c == '_'))
            c = '?';
        tmp[n++] = (char)c;
    }
    size_t a = 0, b = n;
    while (a < b && tmp[a] == ' ') a++;
    while (b > a && tmp[b - 1] == ' ') b--;
    size_t len = b - a;
    if (len > NHL94_TAGNAME_MAX) len = NHL94_TAGNAME_MAX;
    while (len > 0 && tmp[a + len - 1] == ' ') len--;    /* truncation may leave a space */
    if (len >= cap) len = cap - 1;
    memcpy(out, tmp + a, len);
    out[len] = 0;
    return (int)len;
}

void nhl94_tagname_for_seat(int seat, int local_seat, const char *local_name,
                            const NhlTagMember *members, int n, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    for (int i = 0; members && i < n; i++) {
        if (members[i].spectator || members[i].slot != seat) continue;
        if (nhl94_tagname_sanitize(members[i].name, out, cap) > 0) return;
        break;                                  /* seated but unnamed: fall through */
    }
    if (seat == local_seat && nhl94_tagname_sanitize(local_name, out, cap) > 0) return;
    snprintf(out, cap, "P%d", seat + 1);
}

void nhl94_tagname_for_seat_build(int seat, int local_seat, const char *local_name,
                                  const NhlTagMember *members, int n, const char *build_name,
                                  char *out, size_t cap)
{
    if (out && cap && nhl94_tagname_sanitize(build_name, out, cap) > 0) return;
    nhl94_tagname_for_seat(seat, local_seat, local_name, members, n, out, cap);
}
