/*
 * Regression test for the newlib/Zephyr POSIX sysconf() ABI clash
 * (Zephyr issue 111011, patches/zephyr_sysconf_newlib_abi.patch).
 *
 * Prebuilt full-newlib dlmalloc calls sysconf(8) — newlib's _SC_PAGESIZE.
 * Zephyr's POSIX sysconf() must return the page size for that value, not
 * _POSIX_VERSION (200809): a non-power-of-2 page size corrupts dlmalloc's
 * page-align math and the first malloc() can return NULL depending on _end.
 *
 * No console needed — flash, then read the globals via the Debugprobe:
 *   gdb: p g_done; p g_sysconf_ret; p g_expected; p g_malloc_ret
 * PASS = g_done==1 && g_sysconf_ret==g_expected && g_malloc_ret!=0
 */
#include <stdlib.h>
#include <unistd.h>

#include <zephyr/kernel.h>

/* newlib's sys/unistd.h value, baked into the prebuilt libc.a */
#define NEWLIB_SC_PAGESIZE 8

volatile long g_sysconf_ret = -42;
volatile long g_expected = -42;
volatile void *g_malloc_ret;
volatile int g_done;

int main(void)
{
	/* volatile defeats constant folding: force the run-time call ABI,
	 * exactly what newlib's prebuilt dlmalloc does
	 */
	volatile int code = NEWLIB_SC_PAGESIZE;

	g_sysconf_ret = sysconf(code);
	g_expected = (long)CONFIG_POSIX_PAGE_SIZE;
	/* first malloc in the image: newlib dlmalloc queries the page size */
	g_malloc_ret = malloc(16);
	g_done = 1;

	while (1) {
		k_msleep(100);
	}

	return 0;
}
