#include "dfu_cmd.h"

#include <ctype.h>

int dfu_cmd_is_trigger(const char *line)
{
	static const char verb[] = "dfu";

	if (line == 0) {
		return 0;
	}
	while (*line == ' ' || *line == '\t') {     /* skip leading whitespace */
		line++;
	}
	for (int i = 0; i < 3; i++) {               /* case-insensitive "dfu" */
		if (tolower((unsigned char)line[i]) != verb[i]) {
			return 0;
		}
	}
	const char *p = line + 3;                   /* only trailing ws may follow */
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	return (*p == '\0') ? 1 : 0;
}
