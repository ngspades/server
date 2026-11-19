/* Compiled as C so the test proves ngmap.h is usable from the C programs. */
#include "../ngmap.h"

#include <stdlib.h>

int ngmap_c_api_check(void);

int ngmap_c_api_check(void) {
    uint8_t* out = NULL;
    size_t size = 0;
    int w = 0, h = 0, d = 0;
    static const uint8_t junk[4] = {1, 2, 3, 4};
    int r = ngmap_decode(junk, sizeof(junk), &out, &size, &w, &h, &d);
    free(out);
    if (r == NGMAP_OK || out != NULL)
        return 1;
    return ngmap_strerror(r)[0] ? 0 : 1;
}
