// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Lumia 950 (MSM8992, Snapdragon 808) board constants for the eMMC path.
//
// Source for every value below: the mainline MSM8992 device tree, which is
// msm8994.dtsi with two A57 cores removed. Node `sdhc1`, alias `mmc1`:
//
//   mmc@f9824900 {
//       compatible = "qcom,msm8994-sdhci", "qcom,sdhci-msm-v4";
//       reg = <0xf9824900 0x1a0>, <0xf9824000 0x800>;
//       reg-names = "hc", "core";
//       interrupts = <GIC_SPI 123 IRQ_TYPE_LEVEL_HIGH>,
//                    <GIC_SPI 138 IRQ_TYPE_LEVEL_HIGH>;
//       interrupt-names = "hc_irq", "pwr_irq";
//       clocks = <&gcc GCC_SDCC1_AHB_CLK>, <&gcc GCC_SDCC1_APPS_CLK>, <&xo_board>;
//       bus-width = <8>;
//       non-removable;
//   };
//
// These are the SoC's addresses for the controller, not proof of what the Lumia
// firmware programmed. Confirm them on hardware against the firmware's own
// SDCC driver and the ACPI tables copied from the stock FFU before trusting a
// read failure as a driver bug. See README.md, "Proving the map".
#ifndef NATIVE_SDHCI_BOARD_CONSTANTS_H
#define NATIVE_SDHCI_BOARD_CONSTANTS_H

#include "SdhciTypes.h"

namespace lumia950 {

// SDHCI host registers, the "hc" region of the device tree node.
constexpr sdhci::u64 kSdhciHcPhys   = 0xf9824900ULL;
constexpr sdhci::u32 kSdhciHcSize   = 0x1a0;

// Qualcomm QSDCC vendor window, the "core" region. The DLL and power-control
// registers needed for HS200/HS400 live here; this driver does not touch them
// yet, so the window is mapped only for diagnostics.
constexpr sdhci::u64 kSdhciCorePhys = 0xf9824000ULL;
constexpr sdhci::u32 kSdhciCoreSize = 0x800;

// GICv2 interrupts from the node. The Lumia 950's controller is a
// "qcom,msm-qgic2" at 0xf9000000/0xf9002000, not a GICv3 with redistributors.
constexpr sdhci::u32 kIrqHc           = 123;
constexpr sdhci::u32 kIrqPower        = 138;

// Card clock source: XO is 19.2 MHz and GCC supplies SDCC1, but the firmware
// leaves the controller clocked and the card selected. The first bring-up rung
// therefore never reprograms the clock; it measures what is there instead.
constexpr sdhci::u32 kXoHz = 19200000;

// Logical block size reported to IOBlockStorageDriver. eMMC logical sectors are
// 512 bytes; transfers are performed in 512-byte blocks for now.
constexpr sdhci::u32 kLogicalBlockBytes = 512;

// eMMC relative card address. The firmware assigned one and selected the card;
// these are the values EDK2 and Linux hosts normally use, tried in order.
constexpr sdhci::u16 kRcaCandidates[] = { 1, 2 };

// The GPT header sits in LBA 1 with the signature "EFI PART". Reading it is the
// cheapest end-to-end proof that the command path works on real silicon.
constexpr sdhci::u64 kGptHeaderLba = 1;
constexpr char kGptSignature[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

} // namespace lumia950

#endif // NATIVE_SDHCI_BOARD_CONSTANTS_H
