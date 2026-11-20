# GDM7243 pinctrl reconstruction and Linux 6.12 port

## Provenance

The corrected Polish P4 application CPU BE8 kernel ELF is stored on Acer
at `/home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf`.
Its `gdm7243_init()` (0xd04260ec) registers 63 pinmux group
descriptors through `gdm_register_muxes()`. The pointer array is at
0xd044c324 (63 big-endian pointers). Original `mux_set()` at 0xd0019f20
implements masked read-modify-write into the selected bank's mux word.

`tools/recover-pinctrl.py` reconstructs a deterministic sanitized
data header containing **63 groups, 177 options, 42 functions and
131 physical pins**. Only the sanitized C data is checked in;
the proprietary ELF remains external.

To verify that the checked-in data still matches the original:

```sh
uv run --no-project python tools/recover-pinctrl.py   /home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf   target/linux/gdm7243/files/drivers/pinctrl/pinctrl-gdm7243-data.h --check
```

Each OEM descriptor has a group name at +0x10, original first/last
pin at +0x14/+0x18 (encoded as bank << 8 | pin). Its option
slots are at +0x20 + N*0xf8, with function name at +0x04,
function instance ID at +0x08, physical selector register
at +0x0c, mask at +0x10 and value at +0x14.

## Physical selector registers

| Bank | Linux GPIO pins | Register |
| --- | --- | --- |
| GPA | 0–31 | 0xfc000500 |
| GPB | 32–63 | 0xfc000504 |
| GPC | 64–95 | 0xfc000508 |
| GPD | 96–127 | 0xfc00050c |
| GPE | 128–159 | 0xfc000510 |
| GPF | 160–191 | 0xfc000514 |
| GPG | 192–223 | 0xfc000518 |

One selector bitfield can control a multi-pin group. The port
therefore uses Linux pinctrl/pinmux, with GPIO ownership and
mux group reference counts; it does not invent per-pin independent
controls. Function names strip the original `gdm-` prefix; UART
instance IDs are exposed as `uart0`–`uart3`. Normalized group
names include `gpg-9-10` and `gpe-12-15`.

Two vendor-table anomalies have deliberate handling:

1. `gpe-0-10` and `gpe-12-15` address the **same**
   GPE selector bits [2:0], controlling RGMII/MII/RMII and other
   whole-bank functions. The driver rejects conflicting selections
   with `-EBUSY`, while allowing compatible identical encodings.
2. GPF1–3 GPIO options have source `value=1` despite masks
   0x2, 0x4 and 0x8; the OEM RMW would inadvertently set GPF0.
   The generator verifies that exact anomaly before normalizing
   those three GPIO modes to `value=mask`. All remaining entries
   pass a value-within-mask check.

The OEM has some options whose masks cover fewer bits than other
options for that group, notably GPD SDDEV vs GPIO. The port uses
the source masks; unknown extra bit updates are not presumed safe.

## GPIO mapping and unspecified pads

The OEM descriptor range covers **131 of 224** pins in the seven
banks. The other 93 pins have no recovered pinmux option.
The pinctrl driver rejects GPIO requests for those unsupported
offsets. The board DTS should declare:

```dts
gpio-ranges = <&pinctrl 0 0 224>;
gpio-reserved-ranges = <8 24>, <46 18>, <81 15>,
                       <120 8>, <145 15>, <184 8>,
                       <219 5>;
```

GPIO and pinctrl offsets are both `bank*32+pin`.
The GPIO controller can access up to 16 banks in its data
registers, but this pinctrl table documents only banks 0–6.

## Sample device-tree nodes

```dts
pinctrl: pinctrl@fc000500 {
    compatible = "gct,gdm7243-pinctrl";
    reg = <0xfc000500 0x1c>;

    uart0_default: uart0-pins {
        function = "uart0";
        groups = "gpg-9-10";
    };

    rgmii_default: ethernet-pins {
        function = "rgmii";
        groups = "gpe-0-10", "gpe-12-15";
    };
};
```

Connect the UART consumer via `pinctrl-names = "default"` and
`pinctrl-0 = <&uart0_default>`. Ethernet likewise takes
`<&rgmii_default>`. This controller's 0x1c MMIO range directly
follows the existing clock controller at 0xfc000000..0xfc0004ff
and does not overlap it.

## Integration and validation

- Pinmux driver: `target/linux/gdm7243/files/drivers/pinctrl/pinctrl-gdm7243.c`
- Generated sanitized group/mode data: `pinctrl-gdm7243-data.h`
- DT binding: `Documentation/devicetree/bindings/pinctrl/gct,gdm7243-pinctrl.yaml`
- Linux 6.12 Kconfig/Makefile integration patch:
  `0008-pinctrl-gdm7243-build-integration.patch`
- The reproducible `tools/build-drivers.sh` detects and enables
  `CONFIG_PINCTRL_GDM7243=y` alongside the seven prior drivers.

A hardware **RAM-only** boot must still verify actual UART, GPIO,
ethernet mux behavior and shared selector transitions, as well
as which RF/PA/LNA pins are controlled by modem firmware.
No NAND writes or firmware reprogramming are performed.
