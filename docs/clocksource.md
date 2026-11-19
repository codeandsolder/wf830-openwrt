# Always-on GDM7243 RTC counter

Verified from live Polish P4 BE8 Linux, not a related SoC variant:

- `gdm_read_sched_clock` at `d00182e4` reads **physical**
  `0xfc000604` and immediately returns that 32-bit word.
- `gdm_init_time` at `d0426a5c..d0426a6c` invokes
  `setup_sched_clock(..., 32, 32768)`.
- Live boot prints `RTC: secondary(external) 32.768KHz osc` and
  `Switching to clocksource rtccs`.
- The same register block is within the stock kernel's
  identity-mapped `0xfc000000` IO window.

The new Linux driver registers the read-only 32-bit counter with
`clocksource_mmio_init` and `sched_clock_register`. It does not
mutate modem, PM, or RTC control registers.

**Separate unsolved driver:** Linux periodic tick comes from hardware
IRQ 18 and `0xfc006c40`/`0xfc006c50`, initialized by
`gdm_init_time`. The OEM `gdm_set_next_event` is a logging stub;
it did *not* implement arbitrary one-shot timer programming. Do not
pretend the always-on clocksource supplies clock events.

Hardware verification remains: monotonicity across sleep/PLL change,
wraparound, and counter stability before/after baseband startup.
