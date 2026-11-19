# GCT GDM7243 GPIO — source and implementation checkpoint

The genuine P4 2018 BE8 kernel has direct MMIO functions for pin direction,
sampled value and output latch; this clean-room port uses standard gpiolib.

## Verified register map

| Function | P4 symbol | Register |
|---|---|---|
| Direction input | gpio_direction_input at 0xd0019284 | 0xfc002080 + 4*bank |
| Direction output | gpio_direction_output at 0xd00192b0 | same direction register |
| Read data | gpio_get_value at 0xd00192f8 | 0xfc0020c0 + 4*bank |
| Write data | gpio_set_value at 0xd0019324 | same data register |
| IRQ mode | gpio_set_irqmode at 0xd00193dc | 0xfc002100 + 4*bank |
| IRQ polarity | gpio_set_polarity at 0xd001940c | 0xfc002140 + 4*bank |

The OEM pin format is **(bank << 8) | bit**, where bit is masked to
five bits. Its gpio_to_gpio0_irq at 0xd0019634 and
gpio_to_gpio1_irq at 0xd00195f4 expressly reject bank > 6,
which identifies seven *interrupt-capable* banks, not necessarily all
GPIO banks. The direction array occupies 0xfc002080..0xfc0020bc
and the data array 0xfc0020c0..0xfc0020fc, allowing up to sixteen
register-addressed banks (0..15) before register families overlap.
Linux's
gpiolib offset is bank*32 + bit, so the optional GPIO count spans
1..512 lines; 224 covers the proven interrupt-capable banks, while
any additional populated banks must be established by board data.

- Direction bit **0 = input, 1 = output**.
- The OEM *writes output data before setting direction to output*.
- Data register is read for input values and written for output values.
- All operations are 32-bit MMIO and bank-local read-modify-write.
- Raw spinlock protects RMW against concurrent GPIO requests/interrupts.
- The DT ngpios count is explicit, with optional reserved ranges using
  the standard GPIO bindings; the physical pad population remains to be
  checked against board initialization.

The gpio parent interrupt handling is a distinct hardware function:
the original kernel uses **IRQ 10 and IRQ 29** for two GPIO interrupt
channels, with separate per-bank mask/polarity/status registers.
The gpiolib GPIO controller here does not invent a single-parent
IRQ mapping or let the old raw character driver leak into the kernel
API; GPIO IRQ-domain integration needs correct dual-parent routing,
per-bank clear sequencing and pinmux ownership.

## Deployment

Port kernel: Linux 6.12 / OpenWrt 25.12.
Source: drivers/gpio/gpio-gdm7243.c
Binding: Documentation/devicetree/bindings/gpio/gct,gdm7243-gpio.yaml

The eventual DT should declare only actually bonded/usable pins or
mark unbonded ranges reserved, and pinmux must be handed to a separate
pinctrl/pad controller. GPIO accesses are meaningful once its bus
clock and pad mux are initialized by the platform.
