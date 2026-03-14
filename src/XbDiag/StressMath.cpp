// =============================================================================
// StressMath.cpp
//
// CPU stress kernels for XbDiag StressTest.
//
//
// Four kernels matching Prime95's one-pass x87 DWT torture-test loop:
//
//   EightRealsSweep      eight_reals_fft_cmn    52 clocks/block   2 fmuls
//   FourComplexSweep     four_complex_fft_cmn   64 clocks/block  12 fmuls
//   FourComplexSquare    four_complex_square   144 clocks/block  32 fmuls
//   FourComplexUnfft     four_complex_unfft_cmn 65 clocks/block  12 fmuls
//
// CPUStress cycle: EightReals -> FourComplex -> FourComplexSquare -> FourComplexUnfft
// This matches Prime95's pass order: forward FFT, pointwise square, inverse FFT.
//
// Per-cycle budget vs Prime95 gwsquare (4096 doubles, one-pass x87 DWT):
//
//   Kernel               Our blocks  Our clocks  P95 clocks  Coverage
//   eight_reals_fft       512 blk     26,624     ~26,600       ~100%
//   four_complex_fft      512 blk     32,768      32,704       ~100%
//   four_complex_square   512 blk     73,728      73,584       ~100%
//   four_complex_unfft    512 blk     33,280      33,215       ~100%
//   Total                            166,400     139,503       ~119%
//
// Working set:
//   sm_data[4096]          32KB  data buffer, fills Coppermine 32KB L1 exactly
//   sm_sincos[512*6]       24KB  sin/cos table, L2-resident (P6 pipeline hides latency)
//
// Sin/cos table layout per 48-byte block entry at [edi]:
//   [edi+ 0]  sin2  (off2 = 0)      [edi+ 8]  cos2  (off2+8)
//   [edi+16]  sin3  (off3 = 16)     [edi+24]  cos3  (off3+8)
//   [edi+32]  sin4  (off4 = 32)     [edi+40]  cos4  (off4+8)
// =============================================================================

//  ⠀⠀⠀⠀⠀⣠⠶⠚⠛⠛⠛⠲⢦⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⠀⠀⠀⣴⠟⠁⠀⠀⠀⠀⠀⠀⠀⠻⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⠀⣠⣾⣷⣄⠀⠀⠀⢀⣠⣤⣤⡀⠀⢿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⢸⣿⡿⢃⣸⡶⠂⢠⣿⣿⡿⠁⣱⠀⢸⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⢸⡏⠉⠩⣏⣐⣦⠀⠛⠦⠴⠚⠁⠀⣸⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⣼⠧⠶⠶⠶⠿⠶⠶⠖⠚⠛⠉⠁⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⣰⠶⠶⡄⠀⠀
//  ⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⢠⡟⠀⠀⢹⠀⠀
//  ⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⢤⢠⡆⠀⢸⡄⠀⠀⠀⠀⠀⠀⢀⡿⠁⠀⠀⡾⠀⠀
//  ⢹⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⠈⡇⠀⠸⣧⣠⠴⠶⠖⠲⢶⡞⠁⠀⢈⡼⢃⠀⠀
//  ⠸⡆⠀⠀⠀⠀⠀⠀⠀⠀⢸⠀⡇⠀⠀⢿⠁⠄⣲⡶⠶⠿⢤⣄⡀⠛⢛⠉⢻⠀
//  ⠀⢿⡀⠀⠀⠀⠀⠀⠀⠀⢸⠠⣇⠀⠀⠀⠀⠊⠁⠀⠀⠀⠀⠀⠙⢦⠈⠙⠓⣆
//  ⠀⠈⢷⡀⠀⠀⠀⠀⠀⢠⠏⡀⣬⣹⣦⠀⠀⠀⠀⠀⠁⠀⠀⠀⠀⠈⡿⠶⠶⠋
//  ⠀⠀⠈⢷⡀⠀⠀⠀⠀⠘⠛⠛⠋⠀⠀⠀⠀⠀⠀⠄⠀⠀⠀⠀⠀⣼⠃⠀⠀⠀
//  ⠀⠀⠀⠀⠙⢦⣄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠄⠀⠀⣠⡞⠁⠀⠀⠀⠀
//  ⠀⠀⠀⠀⠀⠀⠈⠛⣷⢶⣦⣤⣄⣀⣠⣤⣤⠀⣶⠶⠶⠶⠛⠁⠀⠀⠀⠀⠀⠀
//  ⠀⠀⠀⠀⣀⡀⠀⣰⠇⣾⠀⠀⠈⣩⣥⣄⣿⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⠀⠀⠀⠀⢿⡉⠳⡟⣸⠃⠀⠀⠀⠘⢷⣌⠉⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
//  ⠀⠀⠀⠀⠀⠙⢦⣴⠏⠀⠀⠀⠀⠀⠀⠉⠳⠶⠏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀

#include <xtl.h>
#include <math.h>
#include "StressMath.h"

static const int SM_N = 4096;         // data doubles -- 32KB, fills Coppermine L1
static const int SM_BLOCKS = SM_N / 8;     // 512 blocks per sweep

static double sm_data[SM_N];               // 32KB working buffer
static double sm_sincos[SM_BLOCKS * 6];    // 24KB sin/cos table
static double sm_sqrthalf;                 // sqrt(0.5) for eight_reals kernel
static DWORD  sm_sink = 0;


// =============================================================================
// EightRealsSweep
//
// eight_reals_fft_cmn -- lucas.mac.
// 52 clocks/block, 2 fmul SQRTHALF while all 8 x87 registers are live.
// Processes SM_BLOCKS = 512 blocks per call.  No sincos table.
// =============================================================================

static void EightRealsSweep()
{
    double* end_ptr = sm_data + SM_N;

    __asm
    {
        lea     eax, sm_data
        mov     ecx, end_ptr

        er_loop :

        fninit;; clear FPU before each block

            fld     QWORD PTR[eax + 0]
            fadd    QWORD PTR[eax + 32];; new R1 = R1 + R5
            fld     QWORD PTR[eax + 0]
            fsub    QWORD PTR[eax + 32];; new R5 = R1 - R5
            fld     QWORD PTR[eax + 16]
            fadd    QWORD PTR[eax + 48];; new R3 = R3 + R7
            fld     QWORD PTR[eax + 16]
            fsub    QWORD PTR[eax + 48];; new R7 = R3 - R7
            fld     QWORD PTR[eax + 8]
            fadd    QWORD PTR[eax + 40];; new R2 = R2 + R6
            fld     QWORD PTR[eax + 8]
            fsub    QWORD PTR[eax + 40];; new R6 = R2 - R6
            fld     QWORD PTR[eax + 24]
            fsub    QWORD PTR[eax + 56];; new R8 = R4 - R8
            fxch   st(4);; R3, R6, R2, R7, R8, R5, R1
            fsub   st(6), st;; R1 = R1 - R3
            fadd   st, st;; R3 = R3 * 2
            fld     QWORD PTR[eax + 24]
            fadd    QWORD PTR[eax + 56];; new R4 = R4 + R8  -- all 8 x87 regs live
            fxch   st(2);; R6, R3, R4, R2, R7, R8, R5, R1
            fmul   sm_sqrthalf;; R6 = R6 * SQRTHALF
            fxch   st(1);; R3, R6, R4, R2, R7, R8, R5, R1
            fadd   st, st(7);; R3 = R1 + R3(new R1)
            fxch   st(5);; R8, R6, R4, R2, R7, R3, R5, R1
            fmul   sm_sqrthalf;; R8 = R8 * SQRTHALF
            fxch   st(2);; R4, R6, R8, R2, R7, R3, R5, R1
            fsub   st(3), st;; R2 = R2 - R4(final R4)
            fadd   st, st;; R4 = R4 * 2
            fxch   st(2);; R8, R6, R4, R2, R7, R3, R5, R1
            fsub   st(1), st;; R6 = R6 - R8(Real part)
            fadd   st, st;; R8 = R8 * 2
            fxch   st(3);; R2, R6, R4, R8, R7, R3, R5, R1
            fadd   st(2), st;; R4 = R2 + R4(new R2)
            fxch    st(1);; R6, R4, R2, R8, R7, R1, R5, R3
            fsub    st(6), st;; R5 = R5 - R6(final R7)
            fadd    st(3), st;; R8 = R6 + R8(Imaginary part)
            fadd    st, st;; R6 = R6 * 2
            fxch    st(2);; R2, R4, R6, R8, R7, R1, R5, R3
            fsub    st(5), st;; R1 = R1 - R2(final R2)
            fadd    st, st;; R2 = R2 * 2
            fxch    st(3);; R8, R4, R6, R2, R7, R1, R5, R3
            fsub    st(4), st;; R7 = R7 - R8(final R8)
            fadd    st, st;; R8 = R8 * 2
            fxch    st(6);; R5, R4, R6, R2, R7, R1, R8, R3
            fadd    st(2), st;; R6 = R5 + R6(final R5)
            fxch    st(5);; R1, R4, R6, R2, R7, R5, R8, R3
            fadd    st(3), st;; R2 = R1 + R2(final R1)
            fxch    st(4);; R7, R4, R6, R2, R1, R5, R8, R3
            fadd    st(6), st;; R8 = R7 + R8(final R6)

            fstp    QWORD PTR[eax + 56];; ->R8
            fstp    QWORD PTR[eax + 40];; ->R6
            fstp    QWORD PTR[eax + 16];; ->R3
            fstp    QWORD PTR[eax + 0];; ->R1
            fstp    QWORD PTR[eax + 32];; ->R5
            fstp    QWORD PTR[eax + 24];; ->R4
            fstp    QWORD PTR[eax + 48];; ->R7
            fstp    QWORD PTR[eax + 8];; ->R2

            lea     eax, [eax + 64]
            cmp     eax, ecx
            jb      er_loop
    }
}


// =============================================================================
// FourComplexSweep
//
// four_complex_fft_cmn -- lucas.mac.  off2=0, off3=16, off4=32.
// 64 clocks/block, 12 fmuls/block from sincos table.
// Processes SM_BLOCKS = 512 blocks per call.
// =============================================================================

static void FourComplexSweep()
{
    double* end_ptr = sm_data + SM_N;

    __asm
    {
        lea     eax, sm_data
        lea     edi, sm_sincos
        mov     ecx, end_ptr

        fc_loop :

        fninit;; clear FPU before each block

            fld     QWORD PTR[eax + 16]
            fmul    QWORD PTR[edi + 24];; A3 = R3 * cos3
            fld     QWORD PTR[eax + 48]
            fmul    QWORD PTR[edi + 24];; B3 = I3 * cos3
            fxch    st(1);; A3, B3
            fsub    QWORD PTR[eax + 48];; A3 = A3 - I3
            fld     QWORD PTR[eax + 8]
            fmul    QWORD PTR[edi + 8];; A2 = R2 * cos2
            fxch    st(2);; B3, A3, A2
            fadd    QWORD PTR[eax + 16];; B3 = B3 + R3
            fxch    st(1);; A3, B3, A2
            fmul    QWORD PTR[edi + 16];; A3 = A3 * sin3
            fxch    st(2);; A2, B3, A3
            fsub    QWORD PTR[eax + 40];; A2 = A2 - I2
            fxch    st(1);; B3, A2, A3
            fmul    QWORD PTR[edi + 16];; B3 = B3 * sin3
            fld     QWORD PTR[eax + 40]
            fmul    QWORD PTR[edi + 8];; B2 = I2 * cos2
            fld     QWORD PTR[eax + 24]
            fmul    QWORD PTR[edi + 40];; A4 = R4 * cos4
            fxch    st(1);; B2, A4, B3, A2, A3
            fadd    QWORD PTR[eax + 8];; B2 = B2 + R2
            fxch    st(3);; A2, A4, B3, B2, A3
            fmul    QWORD PTR[edi + 0];; A2 = A2 * sin2
            fld     QWORD PTR[eax + 56]
            fmul    QWORD PTR[edi + 40];; B4 = I4 * cos4
            fxch    st(2);; A4, A2, B4, B3, B2, A3
            fsub    QWORD PTR[eax + 56];; A4 = A4 - I4
            fxch    st(4);; B2, A2, B4, B3, A4, A3
            fmul    QWORD PTR[edi + 0];; B2 = B2 * sin2
            fxch    st(2);; B4, A2, B2, B3, A4, A3
            fadd    QWORD PTR[eax + 24];; B4 = B4 + R4
            fxch    st(4);; A4, A2, B2, B3, B4, A3
            fmul    QWORD PTR[edi + 32];; A4 = A4 * sin4
            fld     QWORD PTR[eax + 0];; R1, A4, A2, B2, B3, B4, A3(7 live)
            fsub    st, st(6);; R1 = R1 - A3
            fxch    st(5);; B4, A4, A2, B2, B3, R1, A3
            fmul    QWORD PTR[edi + 32];; B4 = B4 * sin4
            fld     QWORD PTR[eax + 32];; I1, B4, A4, A2, B2, B3, R1, A3(8 live)
            fsub    st, st(5);; I1 = I1 - B3
            fxch    st(1);; B4, I1, A4, A2, B2, B3, R1, A3
            fsub    st(4), st;; B2 = B2 - B4(new I4)
            fadd    st, st;; B4 = B4 * 2
            fxch    st(2);; A4, I1, B4, A2, I4, B3, R1, A3
            fsub    st(3), st;; A2 = A2 - A4(new R4)
            fadd    st, st;; A4 = A4 * 2
            fxch    st(7);; A3, I1, B4, A2, I4, B3, R1, A4
            fadd    QWORD PTR[eax + 0];; A3 = R1_orig + A3(new R1)
            fxch    st(5);; B3, I1, B4, A2, I4, A3, R1, A4
            fadd    QWORD PTR[eax + 32];; B3 = I1_orig + B3(new I1)
            fxch    st(4);; I4, I1, B4, A2, B3, A3, R1, A4
            fadd    st(2), st;; B4 = I4 + B4(new I2)
            fxch    st(3);; A2, I1, B4, I2, B3, A3, R1, A4
            fadd    st(7), st;; A4 = A2 + A4(new R2)
            fxch    st(3);; I2, I1, B4, A4, B3, A3, R1, R2
            fsub    st(6), st;; R1 = R1 - I2(new R3)
            fadd    st, st;; I2 = I2 * 2
            fxch    st(3);; A4, I1, B4, I2, B3, A3, R1, R2
            fsub    st(1), st;; I1 = I1 - A4(new I4)
            fadd    st, st;; A4 = A4 * 2
            fxch    st(7);; R2, I1, B4, I2, B3, A3, R1, A4
            fsub    st(5), st;; A3 = A3 - R2(new R2)
            fadd    st, st;; R2 = R2 * 2
            fxch    st(2);; B4, I1, R2, I2, B3, A3, R1, A4
            fsub    st(4), st;; B3 = B3 - B4(new I2)
            fadd    st, st;; B4 = B4 * 2
            fxch    st(6);; R1, I1, R2, I2, I2b, A3, B4, A4
            fadd    st(3), st;; I2 = R1 + I2(new R4)
            fxch    st(1);; I1, R1, R2, I2, I2b, A3, B4, A4
            fadd    st(7), st;; A4 = I1 + A4(new I3)
            fxch    st(5);; A3, R1, R2, I2, I2b, I1, B4, A4
            fadd    st(2), st;; R2 = A3 + R2(new R1)
            fxch    st(4);; I2b, R1, R2, I2, R2b, I1, B4, A4
            fadd    st(6), st;; B4 = I2b + B4(new I1)

            fstp    QWORD PTR[eax + 40];; D6
            fstp    QWORD PTR[eax + 16];; D3
            fstp    QWORD PTR[eax + 0];; D1
            fstp    QWORD PTR[eax + 24];; D4
            fstp    QWORD PTR[eax + 8];; D2
            fstp    QWORD PTR[eax + 56];; D8
            fstp    QWORD PTR[eax + 32];; D5
            fstp    QWORD PTR[eax + 48];; D7

            lea     eax, [eax + 64]
            lea     edi, [edi + 48]
            cmp     eax, ecx
            jb      fc_loop
    }
}


// =============================================================================
// FourComplexSquareSweep
//
// four_complex_square -- lucas.mac.
// 144 clocks/block, 32 fmuls/block (twiddle + register-register squaring).
// This is the dominant kernel -- 53% of Prime95's total clock budget.
// Includes two levels of forward FFT, pointwise complex squaring, and two
// levels of inverse FFT, all in one tightly scheduled macro.
// Processes SM_BLOCKS = 512 blocks per call.
//
// Input complex pairs: R1+R5i, R2+R6i, R3+R7i, R4+R8i
//   R1=[eax+0]  R2=[eax+8]   R3=[eax+16]  R4=[eax+24]
//   R5=[eax+32] R6=[eax+40]  R7=[eax+48]  R8=[eax+56]
// =============================================================================

static void FourComplexSquareSweep()
{
    double* end_ptr = sm_data + SM_N;

    __asm
    {
        lea     eax, sm_data
        lea     edi, sm_sincos
        mov     ecx, end_ptr

        fcs_loop :

        fninit;; clear FPU before each block — contains any stack leak to one iteration

            ;; ----forward twiddle : build rotated inputs------------------------ -

            fld     QWORD PTR[eax + 8]
            fmul    QWORD PTR[edi + 0];; A2 = R2 * sin2
            fld     QWORD PTR[eax + 40]
            fmul    QWORD PTR[edi + 0];; B2 = I2 * sin2
            fld     QWORD PTR[eax + 16]
            fmul    QWORD PTR[edi + 16];; A3 = R3 * sin3
            fld     st(1);; C2 = B2
            fmul    QWORD PTR[edi + 8];; C2 = B2 * cos2
            fld     QWORD PTR[eax + 48]
            fmul    QWORD PTR[edi + 16];; B3 = I3 * sin3
            fxch    st(4);; A2, C2, A3, B2, B3
            fadd    st(1), st;; C2 = C2 + A2(new I2)
            fmul    QWORD PTR[edi + 8];; A2 = A2 * cos2
            fld     QWORD PTR[eax + 56]
            fmul    QWORD PTR[edi + 32];; B4 = I4 * sin4
            fxch    st(4);; B2, A2, C2, A3, B4, B3
            fsubp   st(1), st;; A2 = A2 - B2(new R2); pop B2
            fld     QWORD PTR[eax + 24]
            fmul    QWORD PTR[edi + 32];; A4 = R4 * sin4
            fld     st(5);; C3 = B3
            fmul    QWORD PTR[edi + 24];; C3 = B3 * cos3
            fld     st(5);; C4 = B4
            fmul    QWORD PTR[edi + 40];; C4 = B4 * cos4
            fxch    st(5);; A3, C3, A4, A2, C2, C4, B4, B3
            fadd    st(1), st;; C3 = C3 + A3(new I3)
            fmul    QWORD PTR[edi + 24];; A3 = A3 * cos3
            fxch    st(2);; A4, C3, A3, A2, C2, C4, B4, B3
            fadd    st(5), st;; C4 = C4 + A4(new I4)
            fmul    QWORD PTR[edi + 40];; A4 = A4 * cos4
            fxch    st(7);; B3, C3, A3, A2, C2, C4, B4, A4
            fsubp   st(2), st;; A3 = A3 - B3(new R3); pop B3
            fld     QWORD PTR[eax + 32]
            fxch    st(6);; B4, C3, A3, A2, C2, C4, I1, A4
            fsubp   st(7), st;; A4 = A4 - B4(new R4); pop B4
            fld     QWORD PTR[eax + 0];; R1, I3, R3, R2, I2, I4, I1, R4(8 live)

            ;; ----butterfly(forward FFT levels) --------------------------------

            fxch    st(2);; R3, I3, R1, R2, I2, I4, I1, R4
            fsub    st(2), st;; R1 = R1 - R3(new R3)
            fadd    st, st;; R3 = R3 * 2
            fxch    st(1);; I3, R3, R1, R2, I2, I4, I1, R4
            fsub    st(6), st;; I1 = I1 - I3(new I3)
            fadd    st, st;; I3 = I3 * 2
            fxch    st(7);; R4, R3, R1, R2, I2, I4, I1, I3
            fsub    st(3), st;; R2 = R2 - R4(new R4)
            fadd    st, st;; R4 = R4 * 2
            fxch    st(5);; I4, R3, R1, R2, I2, R4, I1, I3
            fsub    st(4), st;; I2 = I2 - I4(new I4)
            fadd    st, st;; I4 = I4 * 2
            fxch    st(2);; R1, R3, I4, R2, I2, R4, I1, I3
            fadd    st(1), st;; R3 = R1 + R3(new R1)
            fxch    st(3);; R2, R3, I4, R1, I2, R4, I1, I3
            fadd    st(5), st;; R4 = R2 + R4(new R2)
            fxch    st(6);; I1, R3, I4, R1, I2, R4, R2, I3
            fadd    st(7), st;; I3 = I1 + I3(new I1)
            fxch    st(4);; I2, R3, I4, R1, I1, R4, R2, I3
            fadd    st(2), st;; I4 = I2 + I4(new I2)
            ;; I4, R1, I2, R3, I3, R2, R4, I1
            fxch    st(5);; R2, R1, I2, R3, I3, I4, R4, I1
            fsub    st(1), st;; R1 = R1 - R2(new R2)
            fadd    st, st;; R2 = R2 * 2
            fxch    st(2);; I2, R1, R2, R3, I3, I4, R4, I1
            fsub    st(7), st;; I1 = I1 - I2(new I2)
            fadd    st, st;; I2 = I2 * 2
            fxch    st(1);; R1, I2, R2, R3, I3, I4, R4, I1

            ;; ----squaring and reconstruction---------------------------------- -

            fst     QWORD PTR[eax + 8];; save intermediate to R2 slot(non - pop)
            faddp   st(2), st;; R2 = R1 + R2(new R1); pop
            fadd    st, st(6);; I2 = I2 + I1(new I1, uses new st(0) = old I2)
            ;; I1, R1, R3, I3, I4, R4, I2(7)
            fxch    st(4);; I4, R1, R3, I3, I1, R4, I2
            fsub    st(2), st;; R3 = R3 - I4(new R3)
            fadd    st, st;; I4 = I4 * 2
            fld     st(1);; TEMP1 = R1
            fxch    st(5);; I1, I4, R1, R3, I3, TEMP1, R4, I2(8)
            fsub    st(5), st;; TEMP1 = TEMP1 - I1(R1 - I1)
            fadd    st, st;; I1 = I1 * 2
            fxch    st(6);; R4, I4, R1, R3, I3, TEMP1, I1, I2
            fsub    st(4), st;; I3 = I3 - R4(new I4)
            fadd    st, st;; R4 = R4 * 2
            fxch    st(6);; I1, I4, R1, R3, I3, TEMP1, R4, I2
            fmul    st(2), st;; R1 = R1 * I1(new I1)
            fadd    st, st(5);; I1 = I1 + TEMP1(R1 + I1)
            fxch    st(1);; I4, I1, R1, R3, I3, TEMP1, R4, I2
            fadd    st, st(3);; I4 = R3 + I4(new R4)
            fxch    st(6);; R4, I1, R1, R3, I3, TEMP1, I4, I2
            fadd    st, st(4);; R4 = I3 + R4(new I3)
            ;; I3, I1, R1, R3, I4, TEMP1, R4, I2
            fxch    st(1);; I1, I3, R1, R3, I4, TEMP1, R4, I2
            fmulp   st(5), st;; TEMP1 = I1 * TEMP1(new R1); pop I1
            fxch    st(1);; I1, I3, R3, I4, R1, R4, I2(7)
            fstp    QWORD PTR[eax + 32];; store new I1 to R5 slot; pop
            fld     st(1);; TEMP3 = R3
            fxch    st(1);; I3, TEMP3, R3, I4, R1, R4, I2(7)
            fsub    st(1), st;; TEMP3 = TEMP3 - I3(R3 - I3)
            fadd    st, st;; I3 = I3 * 2
            fld     st(5);; TEMP4 = R4
            fxch    st(4);; I4, I3, TEMP3, R3, TEMP4, R1, R4, I2(8)
            fsub    st(4), st;; TEMP4 = TEMP4 - I4(R4 - I4)
            fadd    st, st;; I4 = I4 * 2
            fxch    st(1);; I3, I4, TEMP3, R3, TEMP4, R1, R4, I2
            fmul    st(3), st;; R3 = R3 * I3(new I3)
            fadd    st, st(2);; I3 = I3 + TEMP3(R3 + I3)
            fxch    st(1);; I4, I3, TEMP3, R3, TEMP4, R1, R4, I2
            fmul    st(6), st;; R4 = R4 * I4(new I4)
            fadd    st, st(4);; I4 = I4 + TEMP4(R4 + I4)
            fxch    st(1);; I3, I4, TEMP3, R3, TEMP4, R1, R4, I2
            fmulp   st(2), st;; TEMP3 = I3 * TEMP3(new R3); pop I3
            ;; I4, R3, I3, TEMP4, R1, R4, I2(7)
            fld     QWORD PTR[eax + 8];; reload R2(value saved by fst above)
            fxch    st(1);; I4, R2, R3, I3, TEMP4, R1, R4, I2(8)
            fmulp   st(4), st;; TEMP4 = I4 * TEMP4(new R4); pop I4
            ;; R2, R3, I3, R4, R1, I4, I2(7)
            fld     st;; TEMP2 = R2
            fxch    st(7);; I2, R2, R3, I3, R4, R1, I4, TEMP2(8)
            fsub    st(7), st;; TEMP2 = TEMP2 - I2(R2 - I2)
            fadd    st, st;; I2 = I2 * 2
            fxch    st(2);; R3, R2, I2, I3, R4, R1, I4, TEMP2
            fsub    st(4), st;; R4 = R4 - R3(new I4 partial)
            fadd    st, st;; R3 = R3 * 2
            fxch    st(2);; I2, R2, R3, I3, R4, R1, I4, TEMP2
            fmul    st(1), st;; R2 = R2 * I2(new I2)
            fadd    st, st(7);; I2 = I2 + TEMP2(R2 + I2)
            fxch    st(6);; I4, R2, R3, I3, R4, R1, I2, TEMP2
            fsub    st(3), st;; I3 = I3 - I4(new R4 partial)
            fadd    st, st;; I4 = I4 * 2
            fxch    st(6);; I2, R2, R3, I3, R4, R1, I4, TEMP2
            fmulp   st(7), st;; TEMP2 = I2 * TEMP2(new R2); pop I2
            ;; I2, R3, I3, R4, R1, I4, R2(7) --note I2 is old value
            fld     QWORD PTR[eax + 32];; reload I1(saved by fstp earlier)
            fxch    st(1);; I2, I1, R3, I3, R4, R1, I4, R2(8) --I2 = old I2
            fsub    st(1), st;; I1 = I1 - I2(new I2)
            fadd    st, st;; I2 = I2 * 2
            fxch    st(7);; R2, I1, R3, I3, R4, R1, I4, I2
            fsub    st(5), st;; R1 = R1 - R2(new R2 partial)
            fadd    st, st;; R2 = R2 * 2
            fxch    st(4);; R4, I1, R3, I3, R2, R1, I4, I2
            fadd    st(2), st;; R3 = R3 + R4(new R3)
            fxch    st(3);; I3, I1, R3, R4, R2, R1, I4, I2
            fadd    st(6), st;; I4 = I3 + I4(new I3)
            fxch    st(1);; I1, I3, R3, R4, R2, R1, I4, I2
            fadd    st(7), st;; I2 = I1 + I2(new I1)
            fxch    st(5);; R1, I3, R3, R4, R2, I1, I4, I2
            fadd    st(4), st;; R2 = R1 + R2(new R1)
            ;; R2, R4, R3, I4, R1, I2, I3, I1
            fxch    st(1);; R4, R2, R3, I4, R1, I2, I3, I1
            fsub    st(1), st;; R2 = R2 - R4(new R4)
            fadd    st, st;; R4 = R4 * 2
            fxch    st(3);; I4, R2, R3, R4, R1, I2, I3, I1
            fsub    st(5), st;; I2 = I2 - I4(new I4)
            fadd    st, st;; I4 = I4 * 2
            fxch    st(2);; R3, R2, I4, R4, R1, I2, I3, I1
            fsub    st(4), st;; R1 = R1 - R3(new R3)
            fadd    st, st;; R3 = R3 * 2
            fxch    st(6);; I3, R2, I4, R4, R1, I2, R3, I1
            fsub    st(7), st;; I1 = I1 - I3(new I3)
            fadd    st, st;; I3 = I3 * 2

            ;; ----inverse twiddle---------------------------------------------- -

            fxch    st(1);; R2, I3, I4, R4, R1, I2, R3, I1
            fadd    st(3), st;; R4 = R2 + R4(new R2)
            fmul    QWORD PTR[edi + 32];; A4 = R4 * sin4
            fxch    st(5);; I2, I3, I4, R4, R1, A4, R3, I1
            fadd    st(2), st;; I4 = I2 + I4(new I2)
            fmul    QWORD PTR[edi + 32];; B4 = I4 * sin4
            fxch    st(4);; R1, I3, I4, R4, B4, A4, R3, I1
            fadd    st(6), st;; R3 = R1 + R3(new R1)
            fmul    QWORD PTR[edi + 16];; A3 = R3 * sin3
            fxch    st(7);; I1, I3, I4, R4, B4, A4, R3, A3
            fadd    st(1), st;; I3 = I1 + I3(new I1)
            fmul    QWORD PTR[edi + 16];; B3 = I3 * sin3
            ;; B3, I1, I2, R2, B4, A4, R1, A3
            fxch    st(6);; R1, I1, I2, R2, B4, A4, B3, A3
            fstp    QWORD PTR[eax + 0];; store R1; pop
            fstp    QWORD PTR[eax + 32];; store I1 to R5 slot; pop
            fmul    QWORD PTR[edi + 0];; B2 = I2 * sin2
            fld     st(4);; C3 = B3
            fmul    QWORD PTR[edi + 24];; C3 = B3 * cos3
            fld     st(3);; C4 = B4
            fmul    QWORD PTR[edi + 40];; C4 = B4 * cos4
            fxch    st(7);; A3, C3, B2, R2, B4, A4, B3, C4
            fsub    st(1), st;; C3 = C3 - A3(new I3)
            fmul    QWORD PTR[edi + 24];; A3 = A3 * cos3
            fxch    st(5);; A4, C3, B2, R2, B4, A3, B3, C4
            fsub    st(7), st;; C4 = C4 - A4(new I4)
            fmul    QWORD PTR[edi + 40];; A4 = A4 * cos4
            fxch    st(5);; A3, C3, B2, R2, B4, A4, B3, C4
            faddp   st(6), st;; B3 = B3 + A3(new R3); pop A3
            fxch    st(2);; R2, B2, I3, B4, A4, R3, C4(7)
            fmul    QWORD PTR[edi + 0];; A2 = R2 * sin2
            fld     st(1);; C2 = B2
            fmul    QWORD PTR[edi + 8];; C2 = B2 * cos2
            fxch    st(3);; I3, A2, B2, C2, B4, A4, R3, C4(8)
            fstp    QWORD PTR[eax + 48];; store I3 to R7 slot; pop
            fsub    st(2), st;; C2 = C2 - A2(new I2)
            fmul    QWORD PTR[edi + 8];; A2 = A2 * cos2
            fxch    st(5);; R3, B2, C2, B4, A4, A2, C4(7)
            fstp    QWORD PTR[eax + 16];; store R3; pop
            faddp   st(4), st;; A2 = B2 + A2(new R2); pop B2
            fxch    st(2);; A4, B4, I2, R2, C4(5)
            faddp   st(1), st;; B4 = B4 + A4(new R4); pop A4
            ;; R4, I2, R2, I4(4)
            fxch    st(3);; I4, I2, R2, R4
            fstp    QWORD PTR[eax + 56];; store I4 to R8 slot
            fstp    QWORD PTR[eax + 40];; store I2 to R6 slot
            fstp    QWORD PTR[eax + 8];; store R2
            fstp    QWORD PTR[eax + 24];; store R4

            lea     eax, [eax + 64]
            lea     edi, [edi + 48]
            cmp     eax, ecx
            jb      fcs_loop
    }
}


// =============================================================================
// FourComplexUnfftSweep
//
// four_complex_unfft_cmn -- lucas.mac.  off2=0, off3=16, off4=32.
// 65 clocks/block, 12 fmuls/block from sincos table.
// Processes SM_BLOCKS = 512 blocks per call.
// =============================================================================

static void FourComplexUnfftSweep()
{
    double* end_ptr = sm_data + SM_N;

    __asm
    {
        lea     eax, sm_data
        lea     edi, sm_sincos
        mov     ecx, end_ptr

        fcu_loop :

        fninit;; clear FPU before each block

            fld     QWORD PTR[eax + 0]
            fsub    QWORD PTR[eax + 16];; R1 - R3 = new R2
            fld     QWORD PTR[eax + 8]
            fadd    QWORD PTR[eax + 24];; I1 + R4 = new I1
            fld     QWORD PTR[eax + 8]
            fsub    QWORD PTR[eax + 24];; I1 - R4 = new I2
            fld     QWORD PTR[eax + 48]
            fadd    QWORD PTR[eax + 32];; R7 + R5 = new R3
            fld     QWORD PTR[eax + 48]
            fsub    QWORD PTR[eax + 32];; R7 - R5 = new I4
            fld     QWORD PTR[eax + 0]
            fadd    QWORD PTR[eax + 16];; R1 + R3 = new R1
            fld     QWORD PTR[eax + 40]
            fsub    QWORD PTR[eax + 56];; I3 - R8 = new R4
            fld     QWORD PTR[eax + 40]
            fadd    QWORD PTR[eax + 56];; I3 + R8 = new I3
            ;; I3, R4, R1, I4, R3, I2, I1, R2
            fxch    st(4);; R3, R4, R1, I4, I3, I2, I1, R2
            fsub    st(2), st;; R1 = R1 - R3(new R3)
            fadd    st, st;; R3 = R3 * 2
            fxch    st(4);; I3, R4, R1, I4, R3, I2, I1, R2
            fsub    st(6), st;; I1 = I1 - I3(new I3)
            fadd    st, st;; I3 = I3 * 2
            fxch    st(1);; R4, I3, R1, I4, R3, I2, I1, R2
            fsub    st(7), st;; R2 = R2 - R4(new R4)
            fadd    st, st;; R4 = R4 * 2
            fxch    st(3);; I4, I3, R1, R4, R3, I2, I1, R2
            fsub    st(5), st;; I2 = I2 - I4(new I4)
            fadd    st, st;; I4 = I4 * 2
            fxch    st(2);; R1, I3, I4, R4, R3, I2, I1, R2
            fadd    st(4), st;; R3 = R1 + R3(new R1)
            fmul    QWORD PTR[edi + 16];; A3 = R3 * sin3[off3 = 16]
            fxch    st(6);; I1, I3, I4, R4, R3, I2, A3, R2
            fadd    st(1), st;; I3 = I1 + I3(new I1)
            fmul    QWORD PTR[edi + 16];; B3 = I3 * sin3
            fxch    st(7);; R2, I3, I4, R4, R3, I2, A3, B3
            fadd    st(3), st;; R4 = R2 + R4(new R2)
            fmul    QWORD PTR[edi + 32];; A4 = R4 * sin4[off4 = 32]
            fxch    st(5);; I2, I3, I4, R4, R3, A4, A3, B3
            fadd    st(2), st;; I4 = I2 + I4(new I2)
            fmul    QWORD PTR[edi + 32];; B4 = I4 * sin4
            ;; B4, I1, I2, R2, R1, A4, A3, B3
            fxch    st(4);; R1, I1, I2, R2, B4, A4, A3, B3
            fstp    QWORD PTR[eax + 0];; D1
            fstp    QWORD PTR[eax + 8];; D2
            fmul    QWORD PTR[edi + 0];; B2 = I2 * sin2[off2 = 0]
            fld     st(5);; C3 = B3
            fmul    QWORD PTR[edi + 24];; C3 = B3 * cos3[off3 + 8 = 24]
            fld     st(3);; C4 = B4
            fmul    QWORD PTR[edi + 40];; C4 = B4 * cos4[off4 + 8 = 40]
            fxch    st(6);; A3, C3, B2, R2, B4, A4, C4, B3
            fsub    st(1), st;; C3 = C3 - A3(new I3)
            fmul    QWORD PTR[edi + 24];; A3 = A3 * cos3
            fxch    st(5);; A4, C3, B2, R2, B4, A3, C4, B3
            fsub    st(6), st;; C4 = C4 - A4(new I4)
            fmul    QWORD PTR[edi + 40];; A4 = A4 * cos4
            fxch    st(5);; A3, C3, B2, R2, B4, A4, C4, B3
            faddp   st(7), st;; B3 = B3 + A3(new R3); pop A3
            fxch    st(2);; R2, B2, I3, B4, A4, R3, C4(7)
            fmul    QWORD PTR[edi + 0];; A2 = R2 * sin2
            fld     st(1);; C2 = B2
            fmul    QWORD PTR[edi + 8];; C2 = B2 * cos2[off2 + 8 = 8]
            fxch    st(3);; C3, A2, B2, C2, B4, A4, R3, C4(8)
            fstp    QWORD PTR[eax + 40];; D6; pop
            fsub    st(2), st;; C2 = C2 - A2(new I2)
            fmul    QWORD PTR[edi + 8];; A2 = A2 * cos2
            fxch    st(6);; B3, B2, C2, B4, A4, C4, A2(7...wait R3 was stored)
            fstp    QWORD PTR[eax + 32];; D5; pop
            faddp   st(5), st;; A2 = B2 + A2(new R2); pop B2
            fxch    st(2);; A4, B4, I2, R2, C4(5)
            faddp   st(1), st;; B4 = B4 + A4(new R4); pop A4
            ;; R4, I2, R2, I4(4)
            fxch    st(2);; I4, I2, R2, R4 ... wait, C4 is I4
            fstp    QWORD PTR[eax + 56];; D8
            fstp    QWORD PTR[eax + 24];; D4
            fstp    QWORD PTR[eax + 48];; D7
            fstp    QWORD PTR[eax + 16];; D3

            lea     eax, [eax + 64]
            lea     edi, [edi + 48]
            cmp     eax, ecx
            jb      fcu_loop
    }
}


// =============================================================================
// Reseed -- periodic LCG buffer refresh
//
// After enough butterfly passes values can drift toward zero, making butterfly
// ops trivially cheap.  LCG reseed guarantees bounded, non-degenerate,
// non-uniform values.  sm_sink advances state so successive reseeds differ.
// =============================================================================

static const int RESEED_CYCLES = 16;

static void Reseed()
{
    DWORD lcg = sm_sink;
    for (int i = 0; i < SM_N; ++i)
    {
        lcg = lcg * 1664525UL + 1013904223UL;
        sm_data[i] = (double)(int)lcg * (1.0 / 2147483648.0);
    }
    sm_sink = lcg;
}


// =============================================================================
// StressMath_Init
//
// Sets sm_sqrthalf, builds the sin/cos table, seeds sm_data.
// Call once from StressTest_OnEnter.
// =============================================================================

void StressMath_Init()
{
    sm_sqrthalf = sqrt(0.5);

    {
        double twoPiOverN = 6.28318530717958647692 / (double)SM_BLOCKS;
        for (int i = 0; i < SM_BLOCKS; ++i)
        {
            double th = (double)i * twoPiOverN;
            double* e = &sm_sincos[i * 6];
            e[0] = sin(th);
            e[1] = cos(th);
            e[2] = sin(2.0 * th);
            e[3] = cos(2.0 * th);
            e[4] = sin(3.0 * th);
            e[5] = cos(3.0 * th);
        }
    }

    {
        DWORD lcg = 0x12345678UL;
        for (int i = 0; i < SM_N; ++i)
        {
            lcg = lcg * 1664525UL + 1013904223UL;
            sm_data[i] = (double)(int)lcg * (1.0 / 2147483648.0);
        }
        sm_sink = lcg;
    }
}


// =============================================================================
// CPUStress
//
// Burn CPU until GetTickCount() >= deadline.
// Cycle mirrors Prime95 gwsquare pass order:
//   EightRealsSweep     forward FFT, first pass
//   FourComplexSweep    forward FFT, middle passes
//   FourComplexSquare   pointwise squaring (combined forward/square/inverse)
//   FourComplexUnfft    inverse FFT
// Every RESEED_CYCLES cycles, reseed sm_data via LCG to prevent value drift.
// =============================================================================

void CPUStress(DWORD deadline)
{
    int cycle = 0;
    while (GetTickCount() < deadline)
    {
        EightRealsSweep();
        FourComplexSweep();
        FourComplexSquareSweep();
        FourComplexUnfftSweep();

        // Anti-elision: 8 samples spread evenly across 32KB buffer.
        // SM_N=4096 doubles = 8192 DWORDs; stride = 1024 DWORDs = 8KB.
        {
            volatile DWORD* raw = (volatile DWORD*)sm_data;
            sm_sink ^= raw[0] ^ raw[1024] ^ raw[2048] ^ raw[3072]
                ^ raw[4096] ^ raw[5120] ^ raw[6144] ^ raw[7168];
        }

        if (++cycle >= RESEED_CYCLES)
        {
            Reseed();
            cycle = 0;
        }
    }
}