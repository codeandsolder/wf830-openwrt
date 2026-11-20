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

## Two-parent GPIO IRQ chip

The original kernel exposes **root hardware IRQ 10 (GPIO0)** and
**IRQ 29 (GPIO1)**. Both can monitor banks 0..6, with independent masks
and per-pin pending registers, but with common mode/polarity words:

| Function | GPIO0 physical | GPIO1 physical |
| --- | --- | --- |
| Per-bank IRQ mask (RMW) | 0xfc002240 + bank*4 | 0xfc002280 + bank*4 |
| Per-bank pending (W1C) | 0xfc002340 + bank*4 | 0xfc002380 + bank*4 |
| Bank pending summary (W1C) | 0xfc002184 | 0xfc002188 |

Shared mode at **0xfc002100 + bank*4**, polarity at
**0xfc002140 + bank*4**. The OEM `request_gpio{0,1}_irq`
set mode bit for low-nibble 0 (level) and cleared it for nonzero
(edge). Polarity bit was set when the high nibble of the request
flags equaled 0x10 (active high). We translate these cases to
Linux level-high/low and edge-rising/falling; both-edge behavior
requires OEM GPIO0's additional sample/polarity dance and is not
advertised yet.

The per-channel mask helpers `mask_gpio0_irq` at 0xd0019bcc and
`mask_gpio1_irq` at 0xd0019b68 preserve other mask bits and update
one bit at a time. `gpio_intr_ismine` at 0xd0019490 verifies the
bank's summary bit and individual per-pin pending bit for the
correct channel. `gpio_clear_interrupt` at 0xd0019558 snapshots and
acks all seven W1C pending words followed by the bank summary word.

The modern driver uses a normal gpiolib irqchip and two chained
parent IRQ handlers. Every parent callback snapshots pending banks,
clears pending words then acknowledges the bank summary, and routes
only enabled lines on that channel to the Linux child IRQ domain.
The gpiolib valid IRQ mask excludes GPIO lines >= 224 even if the
full data controller exposes more banks.

**Two separate channels can interrupt for the same physical pin,
but Linux gpiolib associates one IRQ with one GPIO line.** The
board-fixed seven-word `gct,irq1-mask` property assigns each line's
IRQ to GPIO1 where bit=1, or GPIO0 otherwise. Both channels are
requested, independent of whether some banks have no pins assigned.
The driver never changes this routing while running. GPIO inputs
that should not produce interrupts should use pinctrl or
`gpio-reserved-ranges` and be left masked.

The MMIO region needs at least **0x320 bytes from 0xfc002080**
to cover the GPIO1 bank6 pending word at 0xfc002398. The former
0x80-byte sample region only covered GPIO direction/data.

Pinmux and actual board pin population remain the responsibility of
a separate pinctrl/pad driver and board DT. The new parent/child
irqdomain mechanism is compile validated only; pending semantics,
mask polarity, level and edge behavior and concurrent acknowledgements
should be verified on the device with a RAM-only kernel boot.

## Deployment

Port kernel: Linux 6.12 / OpenWrt 25.12.
Source: drivers/gpio/gpio-gdm7243.c
Binding: Documentation/devicetree/bindings/gpio/gct,gdm7243-gpio.yaml

The eventual DT should declare only actually bonded/usable pins or
mark unbonded ranges reserved, and pinmux must be handed to a separate
pinctrl/pad controller. GPIO accesses are meaningful once its bus
clock and pad mux are initialized by the platform.
