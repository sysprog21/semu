#ifndef __TEST_H__
#define __TEST_H__

#define TEST_PASS 0
#define TEST_FAIL -1

struct testdata {
    int result;
    const char *name;
    const char *file_path;
};

#define ADD_INSN_TEST(op)                           \
    {                                               \
        .result = TEST_FAIL, .name = #op,           \
        .file_path = "tests/riscv-tests-data/" #op, \
    }

extern struct testdata riscv_tests[];
extern const int n_riscv_tests;

void print_test_result(void);

#endif
