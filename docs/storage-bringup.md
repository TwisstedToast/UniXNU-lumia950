# Lumia 950 storage transport

The read-only root-volume path now shares a freestanding command engine between
the XNU wrapper and host tests. This is implementation and simulated validation,
not evidence of an on-phone storage read or an iOS boot.

## Implemented

- Explicit-width, volatile, barrier-ordered SDHCI MMIO; both mapped windows are
  length-checked. No clock, voltage, bus-width, DLL, or vendor power writes.
- Bounded CMD/DAT reset, inhibition, command, buffer, transfer, and card-state
  waits. Polling status is enabled separately from disabled interrupt signaling.
- Controller error checks, including error bits without the summary bit. R1
  error status is not mistaken for a successful response.
- Reversed and shifted SDHCI R2 registers; modern eMMC CSD structure 3 is valid.
  MMC capacity decoding no longer applies an SDHC-like fixed multiplier.
- Warm CMD13 identification at RCA 1/2, CMD7 deselection, CMD9 while in standby,
  reselection, and CMD8 EXT_CSD. R2 does not enable opcode checking.
- User-partition, 512-byte, high-capacity, command-queue-disabled qualification.
  Unsupported configurations are refused without CMD6 partition/config changes.
- Single-block CMD17 PIO. Transfer-complete may arrive after the last FIFO word;
  it is polled with the same whole-block budget instead of sampled once.
- Data is copied to the caller only after completion without errors. A failed
  read invalidates the attached medium; there is no unqualified transparent retry.
- The XNU wrapper requires an actual GPT-signature read before publishing media.
  A signature is not a GPT CRC or partition-table validation.

## Validation

Run `bash scripts/test-storage.sh`. Both executables use AddressSanitizer and
UndefinedBehaviorSanitizer. The transport test models W1C status, separately
enabled events, command/data delays, R2 encoding, standby/transfer state, wrong
RCA, stuck reset, controller and card errors, partial FIFO failure, missing
completion, destination guards, capacity limits, and unsupported EXT_CSD modes.
The model never performs MMIO or reads the phone.

The portable sources also compile freestanding for AArch64. Separately,
`NativeSdhci.cpp`, `SdhciCore.cpp` and `PollingHost.cpp` compiled with the recorded
XNU/iPhoneOS SDK recipe and linked in
[run 40379567698](https://github.com/TwisstedToast/UniXNU-lumia950/actions/runs/40379567698),
source `c76c869`. This qualifies the build, not an on-phone read or an iOS boot.

## Still required

Qualify the controller's inherited clock/DLL/power state on this handset, identify
the firmware RCA, and read the real GPT. No partition is created or resized by
this work. ADMA, SDHCI IRQ delivery, persistent writes, APFS/Data-volume policy,
root-volume image assembly, and successful launch of iOS userland remain separate
work. A read-only storage device alone does not provide iOS's writable Data volume.

The separate GPU metadata preflight is not evidence of native storage operation.

## Protocol references

Implementation was checked against Linux v6.12 primary sources:

- [SDHCI command, PIO and R2 handling](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/sdhci.c)
- [MMC identification and EXT_CSD decoding](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/core/mmc.c)
- [MMC select/deselect and CSD operations](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/core/mmc_ops.c)
- [MMC status and EXT_CSD fields](https://github.com/torvalds/linux/blob/v6.12/include/linux/mmc/mmc.h)
