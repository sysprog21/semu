#include <stdio.h>

#include "test.h"

struct testdata riscv_tests[] = {
    /* rv64ui-p-* */
    ADD_INSN_TEST(rv64ui_p_add),
    ADD_INSN_TEST(rv64ui_p_addi),
    ADD_INSN_TEST(rv64ui_p_addi),
    ADD_INSN_TEST(rv64ui_p_addiw),
    ADD_INSN_TEST(rv64ui_p_addw),
    ADD_INSN_TEST(rv64ui_p_and),
    ADD_INSN_TEST(rv64ui_p_andi),
    ADD_INSN_TEST(rv64ui_p_auipc),
    ADD_INSN_TEST(rv64ui_p_beq),
    ADD_INSN_TEST(rv64ui_p_bge),
    ADD_INSN_TEST(rv64ui_p_bgeu),
    ADD_INSN_TEST(rv64ui_p_blt),
    ADD_INSN_TEST(rv64ui_p_bltu),
    ADD_INSN_TEST(rv64ui_p_bne),
    ADD_INSN_TEST(rv64ui_p_fence_i),
    ADD_INSN_TEST(rv64ui_p_jal),
    ADD_INSN_TEST(rv64ui_p_jalr),
    ADD_INSN_TEST(rv64ui_p_lb),
    ADD_INSN_TEST(rv64ui_p_lbu),
    ADD_INSN_TEST(rv64ui_p_ld),
    ADD_INSN_TEST(rv64ui_p_lh),
    ADD_INSN_TEST(rv64ui_p_lhu),
    ADD_INSN_TEST(rv64ui_p_lui),
    ADD_INSN_TEST(rv64ui_p_lw),
    ADD_INSN_TEST(rv64ui_p_lwu),
    ADD_INSN_TEST(rv64ui_p_or),
    ADD_INSN_TEST(rv64ui_p_ori),
    ADD_INSN_TEST(rv64ui_p_sb),
    ADD_INSN_TEST(rv64ui_p_sd),
    ADD_INSN_TEST(rv64ui_p_sh),
    ADD_INSN_TEST(rv64ui_p_simple),
    ADD_INSN_TEST(rv64ui_p_sll),
    ADD_INSN_TEST(rv64ui_p_slli),
    ADD_INSN_TEST(rv64ui_p_slliw),
    ADD_INSN_TEST(rv64ui_p_sllw),
    ADD_INSN_TEST(rv64ui_p_slt),
    ADD_INSN_TEST(rv64ui_p_slti),
    ADD_INSN_TEST(rv64ui_p_sltiu),
    ADD_INSN_TEST(rv64ui_p_sltu),
    ADD_INSN_TEST(rv64ui_p_sra),
    ADD_INSN_TEST(rv64ui_p_srai),
    ADD_INSN_TEST(rv64ui_p_sraiw),
    ADD_INSN_TEST(rv64ui_p_sraw),
    ADD_INSN_TEST(rv64ui_p_srl),
    ADD_INSN_TEST(rv64ui_p_srli),
    ADD_INSN_TEST(rv64ui_p_srliw),
    ADD_INSN_TEST(rv64ui_p_srlw),
    ADD_INSN_TEST(rv64ui_p_sub),
    ADD_INSN_TEST(rv64ui_p_subw),
    ADD_INSN_TEST(rv64ui_p_sw),
    ADD_INSN_TEST(rv64ui_p_xor),
    ADD_INSN_TEST(rv64ui_p_xori),

    /* rv64ua-p-* */
    ADD_INSN_TEST(rv64ua_p_amoadd_d),
    ADD_INSN_TEST(rv64ua_p_amoadd_w),
    ADD_INSN_TEST(rv64ua_p_amoand_d),
    ADD_INSN_TEST(rv64ua_p_amoand_w),
    ADD_INSN_TEST(rv64ua_p_amomax_d),
    ADD_INSN_TEST(rv64ua_p_amomax_w),
    ADD_INSN_TEST(rv64ua_p_amomaxu_d),
    ADD_INSN_TEST(rv64ua_p_amomaxu_w),
    ADD_INSN_TEST(rv64ua_p_amomin_d),
    ADD_INSN_TEST(rv64ua_p_amomin_w),
    ADD_INSN_TEST(rv64ua_p_amominu_d),
    ADD_INSN_TEST(rv64ua_p_amominu_w),
    ADD_INSN_TEST(rv64ua_p_amoor_d),
    ADD_INSN_TEST(rv64ua_p_amoor_w),
    ADD_INSN_TEST(rv64ua_p_amoswap_d),
    ADD_INSN_TEST(rv64ua_p_amoswap_w),
    ADD_INSN_TEST(rv64ua_p_amoxor_d),
    ADD_INSN_TEST(rv64ua_p_amoxor_w),
    ADD_INSN_TEST(rv64ua_p_lrsc),
};

const int n_riscv_tests = sizeof(riscv_tests) / sizeof(struct testdata);

#define C_RST "\033[0m"   /* RESET */
#define C_DR "\033[0;31m" /* Dark Red */
#define C_DG "\033[0;32m" /* Dark Green */

void print_test_result(void)
{
    int pass = 0;

    for (int n = 0; n < n_riscv_tests; n++) {
        if (riscv_tests[n].result != TEST_PASS) {
            printf(C_DR "Fail: %s" C_RST "\n", riscv_tests[n].name);
        } else {
            printf(C_DG "Pass: %s" C_RST "\n", riscv_tests[n].name);
            pass++;
        }
    }

    puts("\n=======================");
    printf("Test result: %d/%d\n", pass, n_riscv_tests);
    puts("=======================");
}
