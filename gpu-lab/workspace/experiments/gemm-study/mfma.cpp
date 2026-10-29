#include "../../samples/common/matmul_mfma.hh"

int
main(int argc, char **argv)
{
    return runMfma(argc, argv, USE_LDS ? "mfma_lds" : "mfma_regtiles");
}
