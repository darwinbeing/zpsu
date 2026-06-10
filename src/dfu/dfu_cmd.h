/*
 * Zephyr-free matcher for the UDP `DFU` trigger verb (host-testable, the
 * cred_parse pattern). Kept separate from src/dfu/dfu.c (which is on-target
 * only) so the parse logic can be unit-tested with plain clang.
 */
#ifndef DFU_DFU_CMD_H_
#define DFU_DFU_CMD_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return 1 if `line` is exactly the DFU trigger verb "DFU" (case-insensitive,
 * surrounding spaces/tabs/CR/LF ignored), else 0. NULL returns 0.
 */
int dfu_cmd_is_trigger(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* DFU_DFU_CMD_H_ */
