/* Host test (plain clang, no Zephyr): cred_parse pure logic. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "cred_parse.h"

static void test_validate_ssid(void)
{
	assert(cred_validate_ssid("home") == 0);
	assert(cred_validate_ssid("") != 0);                  /* too short */
	char long_ssid[40];
	memset(long_ssid, 'a', sizeof(long_ssid));
	long_ssid[33] = '\0';                                 /* 33 chars */
	assert(cred_validate_ssid(long_ssid) != 0);           /* > 32 */
}

static void test_validate_psk(void)
{
	assert(cred_validate_psk("12345678") == 0);           /* min 8 */
	assert(cred_validate_psk("1234567") != 0);            /* < 8 */
	char long_psk[80];
	memset(long_psk, 'a', sizeof(long_psk));
	long_psk[64] = '\0';                                  /* 64 chars */
	assert(cred_validate_psk(long_psk) != 0);             /* > 63 */
}

static void test_setap_parse_ok(void)
{
	char ssid[33] = {0}, psk[64] = {0};

	assert(setap_parse("SETAP myssid mypass12", ssid, sizeof(ssid),
			   psk, sizeof(psk)) == 0);
	assert(strcmp(ssid, "myssid") == 0);
	assert(strcmp(psk, "mypass12") == 0);
}

static void test_setap_parse_bad(void)
{
	char ssid[33] = {0}, psk[64] = {0};

	assert(setap_parse("SETAP onlyssid", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* missing psk */
	assert(setap_parse("SETAP a short", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* psk < 8 */
	assert(setap_parse("NOPE x y", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* wrong verb */
}

int main(void)
{
	test_validate_ssid();
	test_validate_psk();
	test_setap_parse_ok();
	test_setap_parse_bad();
	printf("cred_parse: all tests passed\n");
	return 0;
}
