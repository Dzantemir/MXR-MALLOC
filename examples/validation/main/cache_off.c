#include <stdint.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp8266/eagle_soc.h"
#include "mxr_malloc.h"

extern void vPortEnterCritical(void);
extern void vPortExitCritical(void);
extern void Cache_Read_Disable_2(void);
extern void Cache_Read_Enable_2(void);

/* Call only after mxr_init, from task context, on a development board.
 * No logging/asserts/flash strings until cache is restored. */
int IRAM_ATTR mxr_test_cache_off(void)
{
    int result = 0;
    vPortEnterCritical();
    uint32_t nmi = REG_READ(NMI_INT_ENABLE_REG);
    REG_WRITE(NMI_INT_ENABLE_REG, 0);
    Cache_Read_Disable_2();
    uint32_t *p = mxr_malloc_caps(64, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    if (!p) result = 1;
    else {
        for (unsigned i = 0; i < 16; ++i) p[i] = 0x12340000u + i;
        uint32_t *q = mxr_realloc_caps(p, 512, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
        if (!q) { result = 2; mxr_free(p); }
        else {
            for (unsigned i = 0; i < 16; ++i)
                if (q[i] != 0x12340000u + i) result = 3;
            mxr_free(q);
        }
    }
    p = mxr_calloc_caps(8, 4, MALLOC_CAP_32BIT | MXR_CAP_PREFER_IRAM);
    if (p) {
        for (unsigned i = 0; i < 8; ++i) if (p[i]) result = 4;
        mxr_free(p);
    } /* IRAM preference can fail or fall back; inspect on target. */
#if defined(CONFIG_MXR_CANARY) && defined(CONFIG_MXR_DOUBLE_FREE_DETECT)
    p = mxr_malloc_caps(32, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    if (p) {
        ((volatile uint32_t *)p)[-1] ^= 1u; /* deliberately damaged head canary */
        mxr_free(p); /* corrupted block is intentionally retained */
        ((volatile uint32_t *)p)[-1] ^= 1u; /* repair, then release */
        mxr_free(p);
        mxr_free(p); /* intentional invalid free: tests allocator diagnostic */
    }
#endif
    Cache_Read_Enable_2();
    REG_WRITE(NMI_INT_ENABLE_REG, nmi);
    vPortExitCritical();
    return result;
}
