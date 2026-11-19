# GDM7243 periodic system clockevent timer

The Polish P4 live vendor kernel contains a genuine periodic system timer
separate from the 32.768 kHz free-running clocksource. These recovered facts are based
on the OEM BE8 disassembly and the live P4 interrupt snapshot.

## OEM evidence

In **gdm_init_time** at virtual 0xd042696c, the original kernel:
1. Obtains the clock named **timers** from its clock framework and reads its
   rate via clk_get_rate.
2. Derives the reload as rate / 1000 - 1 (the P4 kernel has HZ=1000).
3. Clears control bit 3 at physical 0xfc006c50.
4. Writes the reload at physical 0xfc006c40.
5. Writes control value **0x2c** at physical 0xfc006c50.
6. Installs the handler at hardware IRQ 18.

**gdm_set_next_event** at 0xd0018528 is only a logging stub; it does not
program one-shot ticks. **gdm_set_mode** at 0xd0018548 is likewise a logging
stub. The real tick works because gdm_init_time configures periodic hardware
before registering the clockevent. The live /proc/interrupts contains more
than three million IRQ 18 events.

## Linux 6.12 implementation

The clean-room driver lives at
`target/linux/gdm7243/files/drivers/clocksource/gdm7243-timer.c`;
the DT binding is
`Documentation/devicetree/bindings/timer/gct,gdm7243-timer.yaml`.
OpenWrt applies the Kconfig/Makefile integration as
`0003-clockevent-gdm7243-build-integration.patch`.

- Uses `TIMER_OF_DECLARE` and the standard Linux clockevents API.
- Explicitly advertises **CLOCK_EVT_FEAT_PERIODIC only**; the
  recovered OEM `set_next_event` is a logging stub, not a timer mode.
- The mandatory **clocks** DT phandle supplies the actual
  *timers* clock rate. The driver runs `clk_prepare_enable` and
  derives reload by rounding `rate / HZ` to the nearest cycle,
  then subtracting 1. Stock `HZ=1000` is not hardcoded.
- The clockevent shutdown clears control **bit 3** while preserving
  other bits, reproducing the OEM stop sequence; periodic setup writes
  the reload register, then **0x2c** to the control register.
- The root GDM7243 irqchip performs the observed hardware IRQ
  mask/ack pulse; the timer ISR calls the Linux event handler.
- There is no timer-level acknowledgement in the OEM handler;
  it uses the 32.768 kHz RTC counter for latency instrumentation.
- Resource errors unwind mapped IO, IRQ mapping and clock enabling.

Kernel boot configuration must select **CONFIG_HZ_PERIODIC=y**,
rather than tickless/high-resolution modes that require a supported
one-shot clockevent. The clocksource is a separate 32.768 kHz counter.

The boot log also prints **1x clock = 104160000 Hz**, but it does **not**
establish this to be the effective *timers* clock rate. Recover the clock
divider/PLL configuration rather than assuming that number.

## Board integration and validation

A future board DTS needs the exact actual `timers` clock provider and
a parent interrupt controller mapping IRQ 18. The 1x clock logged by
P4 is a clue, not justification for freezing a clock rate in the
driver; future PLL changes need consideration.

Hardware checks: register-level measurements against clock-source behavior,
IRQ 18 delivery and acknowledgment, 1-second wall-clock drift with selected
kernel HZ values, sleep and clock-gating behavior.
