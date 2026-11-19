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

## Implementation contract for the next driver

The clockevent driver is not yet merged. The planned Linux 6.12
**gdm7243 timer** should use TIMER_OF_DECLARE, the standard
clockevents framework and the IRQ domain from irq-gdm7243.c.

- Advertise only CLOCK_EVT_FEAT_PERIODIC until one-shot support is proven.
- Require a **clocks** DT phandle for the actual *timers* clock rate.
- Derive the reload from the current kernel HZ, not a fixed 1 kHz tick.
- Shutdown must disable control bit 3; periodic setup uses control 0x2c.
- The root IRQ controller supplies the verified mask/ack pulse. Confirm
  whether the timer IP requires further local acknowledgement.
- Keep all timer accesses separate from RTC, modem, NAND and storage.

The boot log also prints **1x clock = 104160000 Hz**, but it does **not**
establish this to be the effective *timers* clock rate. Recover the clock
divider/PLL configuration rather than assuming that number.

## Bring-up requirements

Use a periodic-tick kernel configuration; dynamic tick and high-resolution
clockevents require a verified one-shot source or another timer implementation.

Hardware checks: register-level measurements against clock-source behavior,
IRQ 18 delivery and acknowledgment, 1-second wall-clock drift with selected
kernel HZ values, sleep and clock-gating behavior.
