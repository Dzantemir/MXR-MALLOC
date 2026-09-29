#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "mxr_malloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    /* On-device smoke test; no hardware results are claimed by the ZIP. */
    uint32_t *p = mxr_malloc_caps(64, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    assert(p);
    for (unsigned i = 0; i < 16; ++i) p[i] = 0x12340000u + i;
    p = mxr_realloc_caps(p, 1024, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    assert(p);
    for (unsigned i = 0; i < 16; ++i) assert(p[i] == 0x12340000u + i);
    mxr_free(p);
    void *z = mxr_zalloc_caps(32, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    assert(z); mxr_free(z);
    z = mxr_calloc_caps(8, 4, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    assert(z); mxr_free(z);
    mxr_status_t st;
    mxr_get_status(&st);
    assert(st.initialized);
    assert(st.free_bytes <= st.total_bytes);
#ifdef CONFIG_MXR_TEST_CACHE_OFF
    extern int mxr_test_cache_off(void);
    int result = mxr_test_cache_off();
    printf("cache-off result=%d (0=PASS)\n", result);
    assert(result == 0);
#endif
    mxr_dump();
    printf("MXR on-device smoke PASS; free=%u\n", (unsigned)st.free_bytes);
}
