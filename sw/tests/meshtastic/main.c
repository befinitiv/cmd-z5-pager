#include <stdio.h>

int failures;

void test_crypto(void);
void test_packet(void);
void test_router(void);
void test_nodedb(void);

int main(void)
{
	test_crypto();
	test_packet();
	test_router();
	test_nodedb();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
