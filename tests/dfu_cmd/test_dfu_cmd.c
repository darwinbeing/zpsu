/* Host test (plain clang, no Zephyr): dfu_cmd pure matcher logic. */
#include <assert.h>
#include <stdio.h>
#include "dfu_cmd.h"

static void test_matches(void)
{
	assert(dfu_cmd_is_trigger("DFU") == 1);
	assert(dfu_cmd_is_trigger("dfu") == 1);
	assert(dfu_cmd_is_trigger("Dfu") == 1);
	assert(dfu_cmd_is_trigger("  DFU  ") == 1);     /* surrounding spaces */
	assert(dfu_cmd_is_trigger("\tDFU\r\n") == 1);   /* tab + CRLF */
}

static void test_rejects(void)
{
	assert(dfu_cmd_is_trigger("DF") == 0);          /* too short */
	assert(dfu_cmd_is_trigger("DFUX") == 0);        /* trailing junk */
	assert(dfu_cmd_is_trigger("DFU ON") == 0);      /* extra token */
	assert(dfu_cmd_is_trigger("XDFU") == 0);        /* leading junk */
	assert(dfu_cmd_is_trigger("") == 0);            /* empty */
	assert(dfu_cmd_is_trigger(0) == 0);             /* NULL */
}

int main(void)
{
	test_matches();
	test_rejects();
	printf("dfu_cmd: all tests passed\n");
	return 0;
}
