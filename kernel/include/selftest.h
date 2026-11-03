#ifndef TERN_SELFTEST_H
#define TERN_SELFTEST_H
/* Boot-time self-tests run in test mode; each prints [TEST] markers or panics. */
void test_pmm(void);
void test_vmm(void);
#endif
