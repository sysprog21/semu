#ifndef __TEST_H__
#define __TEST_H__

enum {
    TEST_Unknown = 0,
    TEST_Passed = 1,
    TEST_Failed = -1,
};

struct testdata {
    int result;
    const char *name;
    const char *file_path;
};

#define ADD_INSN_TEST(op)                           \
    {                                               \
        .result = TEST_Unknown, .name = #op,        \
        .file_path = "tests/riscv-tests-data/" #op, \
    }

extern struct testdata riscv_tests[];
extern const int n_riscv_tests;

void print_test_iter_start(int n_test);
void print_test_start(struct testdata *test);
void print_test_end(struct testdata *test, uint64_t a0, uint64_t tohost);
void print_test_iter_end(int n_tested);

#endif
