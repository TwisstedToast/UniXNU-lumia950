// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Lumia 950 (MSM8992, Snapdragon 808) board constants for the GPU path.
//
// Every value below has a source that is not this repository. Mainline Linux
// never describes MSM8992's graphics (msm8994.dtsi in mainline has no kgsl or
// mdss node at all), so the device tree values come from Qualcomm's msm-3.10
// tree for this SoC, published for the LG G4:
//
//   LineageOS/android_kernel_lge_msm8992, branch lineage-15.1,
//   arch/arm/boot/dts/qcom/msm8992-gpu.dtsi and msm-gdsc.dtsi
//
//   qcom,kgsl-3d0@fdb00000 {
//       compatible = "qcom,kgsl-3d0", "qcom,kgsl-3d";
//       reg = <0xfdb00000 0x40000>;
//       reg-names = "kgsl_3d0_reg_memory";
//       interrupts = <0 33 0>;              // GIC SPI 33
//       qcom,chipid = <0x04010800>;         // Adreno 418 is a4xx
//       qcom,initial-pwrlevel = <4>;        // 300 MHz
//       clocks = <&clock_mmss clk_oxili_gfx3d_clk>,
//                <&clock_mmss clk_oxilicx_ahb_clk>,
//                <&clock_mmss clk_oxili_rbbmtimer_clk>;
//       vddcx-supply = <&gdsc_oxili_cx>;    // "cx" domain
//       vdd-supply  = <&gdsc_oxili_gx>;     // "gx" domain
//       qcom,gpu-pwrlevels 0..6: 600, 490, 450, 367, 300, 180, 27 MHz
//   }
//
//   gdsc_oxili_gx: qcom,gdsc@fd8c4024 { reg = <0xfd8c4024 0x4>; };
//   gdsc_oxili_cx: qcom,gdsc@fd8c4034 { reg = <0xfd8c4034 0x4>; };
//
// The GDSC registers live in the MMSS clock controller, whose window is
// 0xfd8c0000 (README.md, "The eMMC map", lists it for the storage path too).
// Their one meaningful read-only bit is powers-on: drivers/clk/qcom/gdsc.c in
// the same tree defines PWR_ON_MASK as BIT(31) and polls exactly that bit.
//
// Reading a GDSCR is the one access this driver makes without a clock vote, on
// the assumption that the MMSS register block is still reachable the way the
// firmware left it. Qualcomm's own GDSC driver does the same, but if the phone
// ever hangs on the first of these two reads, the missing clock vote is the
// answer and that is rung 2, not a reason to read the GPU window instead.
//
// The GPU's own register offsets are dword indices from freedreno's register
// database (Mesa, src/freedreno/registers/adreno/a4xx.xml):
//
//   RBBM_HW_VERSION      0x0000    RBBM_STATUS        0x0191
//   RBBM_HW_CONFIGURATION 0x0002   CP_RB_BASE         0x0200
//   RBBM_RBBM_CTL        0x003e    CP_RB_CNTL         0x0201
//                                  CP_RB_RPTR         0x0204
//                                  CP_RB_WPTR         0x0205
//                                  CP_ME_CNTL         0x022d
//
// Qualcomm's own a4xx header spells the same registers as byte offsets divided
// by four (drivers/gpu/msm/a4xx_reg.h: A4XX_RBBM_STATUS 0x191, A4XX_CP_RB_BASE
// 0x200), which is why kRegisterShift below converts an index to a byte offset.
//
// These are the SoC's addresses, not proof of what the Lumia firmware left
// behind. A domain that the firmware collapsed cannot be read: the AHB window
// answers a collapsed domain badly enough that the SoC can hang, so this driver
// checks the GDSC bit first and refuses to touch the block otherwise.
#ifndef NATIVE_ADRENO418_BOARD_CONSTANTS_H
#define NATIVE_ADRENO418_BOARD_CONSTANTS_H

#include "AdrenoTypes.h"

namespace lumia950 {

// KGSL register window, "kgsl_3d0_reg_memory" in the device tree node.
constexpr adreno::u64 kGpuPhys = 0xfdb00000ULL;
constexpr adreno::u32 kGpuSize = 0x40000;

// MMSS clock controller: where the two OxiLi power domains report their state.
// The driver's ADT node carries this as its second reg window so the mapping
// needs nothing the platform expert does not already publish.
//
// The window is 64 KiB, not 4 KiB: the GDSC registers sit at 0x4024 and 0x4034,
// past the first page, so a 0x1000-byte mapping would leave every GDSCR read
// outside it — a driver that maps a window and then reads past its end gets
// whatever the virtual mapping does next, which on a kernel mapping is not an
// access fault but a wrong answer.
constexpr adreno::u64 kMmssPhys = 0xfd8c0000ULL;
constexpr adreno::u32 kMmssSize = 0x10000;

constexpr adreno::u32 kGdscrGxOffset = 0x4024;   // gdsc_oxili_gx
constexpr adreno::u32 kGdscrCxOffset = 0x4034;   // gdsc_oxili_cx

// drivers/clk/qcom/gdsc.c: the domain is on when this bit reads 1.
constexpr adreno::u32 kGdscrPowerOn = 1u << 31;

// GIC SPI 33, the kgsl_3d0_irq of the device tree node. Nothing enables it in
// this rung; the number is here so the ADT node and the driver agree.
constexpr adreno::u32 kGpuIrq = 33;

// qcom,chipid. Bits 31:24 are the generation; a4xx answers 0x04, which is what
// the driver checks the hardware against.
constexpr adreno::u32 kChipId = 0x04010800;
constexpr adreno::u32 kA4xxGeneration = 0x04;

// qcom,gpu-pwrlevels: the fastest level and the one the device tree boots at.
constexpr adreno::u32 kMaxGpuHz = 600000000;
constexpr adreno::u32 kBootGpuHz = 300000000;
constexpr adreno::u32 kFixedGpuMicrovolts = 980000;
constexpr adreno::u32 kMaximumGpuMicrovolts = 1355000;

// The GPU's IOMMU. Same tree, arch/arm/boot/dts/qcom/msm-iommu-v1.dtsi:
//   kgsl_iommu: qcom,iommu@fdb10000 { reg = <0xfdb10000 0x10000>;
//                                     interrupts = <0 38 0>, ... };
// It sits one window above KGSL. Rung 3 maps it; nothing reads it yet.
constexpr adreno::u64 kGpuSmmuPhys = 0xfdb10000ULL;
constexpr adreno::u32 kGpuSmmuSize = 0x10000;
constexpr adreno::u32 kGpuSmmuPmonIrq = 38;

// GPU registers, as freedreno's dword indices. Byte offset = index << 2.
constexpr adreno::u32 kRegisterShift = 2;
constexpr adreno::u32 kRegRbbmHwVersion = 0x0000;
constexpr adreno::u32 kRegRbbmHwConfiguration = 0x0002;
constexpr adreno::u32 kRegRbbmRbbmCtl = 0x003e;
constexpr adreno::u32 kRegRbbmStatus = 0x0191;
constexpr adreno::u32 kRegCpRbBase = 0x0200;
constexpr adreno::u32 kRegCpRbCntl = 0x0201;
constexpr adreno::u32 kRegCpRbRptr = 0x0204;
constexpr adreno::u32 kRegCpRbWptr = 0x0205;
constexpr adreno::u32 kRegCpMeCntl = 0x022d;
// The command processor's microcode ports (freedreno a4xx.xml). a4xx_gpu.c loads
// PM4 into CP_ME_RAM and the PFP into its own window before it touches the ring,
// so these are the registers that gate rung 4 rather than the ring's own.
constexpr adreno::u32 kRegCpPfpUcodeAddr = 0x0223;
constexpr adreno::u32 kRegCpPfpUcodeData = 0x0224;
constexpr adreno::u32 kRegCpMeRamWaddr = 0x0225;
constexpr adreno::u32 kRegCpMeRamData = 0x0227;

// The a4xx command processor ring must be a power of two and 4 KiB aligned.
// 64 KiB is this driver's choice for the rung that submits work, not a value
// read from the device: nothing here programs CP_RB_BASE yet.
constexpr adreno::u32 kCpRingBytes = 0x10000;
constexpr adreno::u32 kCpRingDwords = kCpRingBytes / 4;

} // namespace lumia950

#endif // NATIVE_ADRENO418_BOARD_CONSTANTS_H
