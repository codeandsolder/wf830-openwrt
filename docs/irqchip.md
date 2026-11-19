# GDM7243QT root interrupt controller — primary evidence

Source: 2018-12-03 live Polish P4 WF830 Linux 3.10.0-uc0, corrected
ARMv7 BE8 disassembly. Do not substitute Indosat/Bolt firmware.

The kernel's IO mapping descriptor at `d043b864` maps virtual
`0xfc000000` to the *same physical* `0xfc000000`, 2 MiB,
as a device mapping. Thus the MMIO addresses here are physical.

## Register model

| Offset, base `0xfc002c00` | Bank 0 | Bank 1 |
| --- | --- | --- |
| Mask / enable bit | `+0x04` | `+0x44` |
| Pending | `+0x0c` | `+0x4c` |
| Mask+ack pulse | `+0x1c` | `+0x5c` |

- `gdm_mask_irq` `d00168a0`: RMW to clear `BIT(hwirq % 32)` in enable word.
- `gdm_unmask_irq` `d00168d8`: RMW to set same bit.
- `gdm_mask_ack_irq` `d0016910`: mask, write bit to ack word,
  then write zero to ack word. The pulse, *not* a write-1-to-clear
  alone, is the observed behavior.
- `gdm_init_irq` `d0425e34`: registers 64 IRQs with
  `handle_level_irq`, explicitly skips IRQ 19.
- `__irq_svc` `d000dc40` and `__irq_usr` `d000de00`:
  read bank 0 pending then bank 1; choose most-significant bit among
  pending bits 0–7 if any, else most-significant pending bit in bank.
  Branch to `asm_do_IRQ`, handling one IRQ per exception.
- `/proc/interrupts` live names/IRQs: `serial0_rx=2`,
  `serial0_tx=3`, `usim=11`, `eth0=15`, `sdhci=16`,
  `timer=18`, `rtctmr=28`, `ipcif=31`.

## Implementation choices

- Root `irq_domain` mapping for 64 hardware IRQ lines; reserved
  hwirq 19 rejected by mapping.
- One dispatched IRQ per ARM exception, matching stock handlers.
- `handle_level_irq` and exact combined mask/ack pulse.
- Driver uses a lock for register read-modify-write operations.
- Clear bootloader enabled masks before registering handlers.
- Only one interrupt cell because original hardware is level-based;
  do not invent unsupported edge/polarity configuration.
- Separate from the kernel's USB-host `gdm724x` staging driver.

## Gates requiring actual board

- Test that the bootloader preserves physical MMIO mapping and the
  status registers reflect *enabled* sources; verify no IRQ storm.
- Observe UART TX/RX IRQ 2/3 then periodic timer IRQ 18.
- Confirm whether status reads need explicit filtering against enable
  masks, whether IRQ 19 can ever assert and whether hardware IRQs have
  undocumented per-line polarity config.
