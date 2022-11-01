#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

/* clang-format off */
struct testdata riscv_tests[] = {
    /* rv64ui-p-* */
    ADD_INSN_TEST(rv64ui-p-add),
    ADD_INSN_TEST(rv64ui-p-addi),
    ADD_INSN_TEST(rv64ui-p-addiw),
    ADD_INSN_TEST(rv64ui-p-addw),
    ADD_INSN_TEST(rv64ui-p-and),
    ADD_INSN_TEST(rv64ui-p-andi),
    ADD_INSN_TEST(rv64ui-p-auipc),
    ADD_INSN_TEST(rv64ui-p-beq),
    ADD_INSN_TEST(rv64ui-p-bge),
    ADD_INSN_TEST(rv64ui-p-bgeu),
    ADD_INSN_TEST(rv64ui-p-blt),
    ADD_INSN_TEST(rv64ui-p-bltu),
    ADD_INSN_TEST(rv64ui-p-bne),
    ADD_INSN_TEST(rv64ui-p-fence_i),
    ADD_INSN_TEST(rv64ui-p-jal),
    ADD_INSN_TEST(rv64ui-p-jalr),
    ADD_INSN_TEST(rv64ui-p-lb),
    ADD_INSN_TEST(rv64ui-p-lbu),
    ADD_INSN_TEST(rv64ui-p-ld),
    ADD_INSN_TEST(rv64ui-p-lh),
    ADD_INSN_TEST(rv64ui-p-lhu),
    ADD_INSN_TEST(rv64ui-p-lui),
    ADD_INSN_TEST(rv64ui-p-lw),
    ADD_INSN_TEST(rv64ui-p-lwu),
    ADD_INSN_TEST(rv64ui-p-or),
    ADD_INSN_TEST(rv64ui-p-ori),
    ADD_INSN_TEST(rv64ui-p-sb),
    ADD_INSN_TEST(rv64ui-p-sd),
    ADD_INSN_TEST(rv64ui-p-sh),
    ADD_INSN_TEST(rv64ui-p-simple),
    ADD_INSN_TEST(rv64ui-p-sll),
    ADD_INSN_TEST(rv64ui-p-slli),
    ADD_INSN_TEST(rv64ui-p-slliw),
    ADD_INSN_TEST(rv64ui-p-sllw),
    ADD_INSN_TEST(rv64ui-p-slt),
    ADD_INSN_TEST(rv64ui-p-slti),
    ADD_INSN_TEST(rv64ui-p-sltiu),
    ADD_INSN_TEST(rv64ui-p-sltu),
    ADD_INSN_TEST(rv64ui-p-sra),
    ADD_INSN_TEST(rv64ui-p-srai),
    ADD_INSN_TEST(rv64ui-p-sraiw),
    ADD_INSN_TEST(rv64ui-p-sraw),
    ADD_INSN_TEST(rv64ui-p-srl),
    ADD_INSN_TEST(rv64ui-p-srli),
    ADD_INSN_TEST(rv64ui-p-srliw),
    ADD_INSN_TEST(rv64ui-p-srlw),
    ADD_INSN_TEST(rv64ui-p-sub),
    ADD_INSN_TEST(rv64ui-p-subw),
    ADD_INSN_TEST(rv64ui-p-sw),
    ADD_INSN_TEST(rv64ui-p-xor),
    ADD_INSN_TEST(rv64ui-p-xori),

    /* rv64um-p-* */
    ADD_INSN_TEST(rv64um-p-div),
    ADD_INSN_TEST(rv64um-p-divu),
    ADD_INSN_TEST(rv64um-p-divuw),
    ADD_INSN_TEST(rv64um-p-divw),
    ADD_INSN_TEST(rv64um-p-mul),
    ADD_INSN_TEST(rv64um-p-mulh),
    ADD_INSN_TEST(rv64um-p-mulhsu),
    ADD_INSN_TEST(rv64um-p-mulhu),
    ADD_INSN_TEST(rv64um-p-mulw),
    ADD_INSN_TEST(rv64um-p-rem),
    ADD_INSN_TEST(rv64um-p-remu),
    ADD_INSN_TEST(rv64um-p-remuw),
    ADD_INSN_TEST(rv64um-p-remw),

    /* rv64ua-p-* */
    ADD_INSN_TEST(rv64ua-p-amoadd_d),
    ADD_INSN_TEST(rv64ua-p-amoadd_w),
    ADD_INSN_TEST(rv64ua-p-amoand_d),
    ADD_INSN_TEST(rv64ua-p-amoand_w),
    ADD_INSN_TEST(rv64ua-p-amomax_d),
    ADD_INSN_TEST(rv64ua-p-amomax_w),
    ADD_INSN_TEST(rv64ua-p-amomaxu_d),
    ADD_INSN_TEST(rv64ua-p-amomaxu_w),
    ADD_INSN_TEST(rv64ua-p-amomin_d),
    ADD_INSN_TEST(rv64ua-p-amomin_w),
    ADD_INSN_TEST(rv64ua-p-amominu_d),
    ADD_INSN_TEST(rv64ua-p-amominu_w),
    ADD_INSN_TEST(rv64ua-p-amoor_d),
    ADD_INSN_TEST(rv64ua-p-amoor_w),
    ADD_INSN_TEST(rv64ua-p-amoswap_d),
    ADD_INSN_TEST(rv64ua-p-amoswap_w),
    ADD_INSN_TEST(rv64ua-p-amoxor_d),
    ADD_INSN_TEST(rv64ua-p-amoxor_w),
    ADD_INSN_TEST(rv64ua-p-lrsc),
};
/* clang-format on */

const int n_riscv_tests = sizeof(riscv_tests) / sizeof(struct testdata);

#define C_RST "\033[0m"   /* RESET */
#define C_DR "\033[0;31m" /* Dark Red */
#define C_DG "\033[0;32m" /* Dark Green */

void print_test_iter_start(int n_test)
{
    printf(C_DG "[==========] " C_RST);
    printf("Running %d test(s) from riscv-tests.\n", n_test);
}

void print_test_start(struct testdata *test)
{
    printf(C_DG "[ RUN      ] " C_RST);
    printf("%s\n", test->name);
}

void print_test_end(struct testdata *test, uint64_t a0, uint64_t tohost)
{
#define TOHOST_EXCEPTION_MAGIC 1337

    if (test->result == TEST_Passed) {
        printf(C_DG "[       OK ] " C_RST);
    } else {
        printf("  a0 = 0x%lx\n", a0);
        printf("  tohost = 0x%lx\n", tohost);
        if (tohost < TOHOST_EXCEPTION_MAGIC)
            printf("  Fail test case = %ld.\n", tohost >> 1);
        else
            printf("  An exception occurred.\n");
        printf(C_DR "[  FAILED  ] " C_RST);
    }
    printf("%s\n", test->name);
}

void print_test_iter_end(int n_tested)
{
    int n_pass = 0;

    printf(C_DG "[==========] " C_RST);
    printf("%d test(s) from riscv-tests ran.\n", n_tested);

    /* Count how many tests are passed. */
    for (int n = 0; n < n_riscv_tests; n++) {
        if (riscv_tests[n].result == TEST_Passed)
            n_pass++;
    }

    printf(C_DG "[  PASSED  ] " C_RST);
    printf("%d test(s).\n", n_pass);

    /* If there are some failed tests, we print them. */
    if (n_pass != n_tested) {
        printf(C_DR "[  FAILED  ] " C_RST);
        printf("%d test(s), listed below:\n", n_tested - n_pass);
        for (int n = 0; n < n_riscv_tests; n++) {
            if (riscv_tests[n].result == TEST_Failed) {
                printf(C_DR "[  FAILED  ] " C_RST);
                printf("%s\n", riscv_tests[n].name);
            }
        }
    }
}
