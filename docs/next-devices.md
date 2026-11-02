# Next devices and host integration

ZxKit emulates the hardware and exposes byte and input interfaces. It has no Qt,
JavaScript, socket or Raspberry Pi emulation dependency. The device registers and
DAC aliases follow the Next FPGA core 3.02.03 (`cores/zxnext/ports.txt`,
`nextreg.txt`, `src/serial/uart.vhd` and `src/zxnext.vhd` in
https://gitlab.com/SpectrumNext/ZX_Spectrum_Next_FPGA).

## UART

`ZxNextMachine::uart()` provides two independent UARTs. Ports 133B, 143B, 153B and
163B implement status/transmit, receive/prescaler, channel selection and framing.
Each UART has a 512-byte receive FIFO, a 64-byte transmit FIFO and a transmit
shift register. Receive overflow clears on status read. Framing selects byte
width and transfer time; reset stays asserted until frame bit 7 is cleared.
`setClearToSend()` controls host CTS when hardware flow control is enabled.
The byte interface does not simulate electrical noise or parity/framing faults.

`setTransmitHandler(fn)` receives completed transmitted bytes, synchronously on
the thread running the machine. `receive(channel, bytes, now)` queues incoming
bytes and returns the number accepted into the bounded 64 KiB wire queue; retain
and retry any remainder. Wire bytes reach the FIFO at the configured baud rate.
The host implements ESP commands, networking or a Pi service. With no host
attached transmission drains normally and no replies are invented.

Time is in base 3.5 MHz T-states, independent of CPU turbo:

```cpp
auto &clock = emulator.clock();
const auto now = clock.frameCount() * uint64_t(clock.timings().tstatesPerFrame())
               + clock.frameBaseTstates();
next.uart().receive(0, incomingBytes, now);
```

The UART hardware state is included in internal state/rewind data; callbacks and
host connections are not. `generation()` changes on reset/restore so consumers
can discard stale connections and replies. Reconnect the handler when the machine
is replaced. Access a shared emulator under its lock.

`EngineConfig::threadHooks` provides `started`, `poll` and `stopped` callbacks on
the Engine thread. Construct thread-affine services in `started` or lazily in
`poll`, and destroy them in `stopped`. These hooks receive an **already locked**
`Emulator &`: do not acquire its lock again. Poll runs once per frame, and on
paused refresh/state changes. A host service must not block on network or file
I/O. Ordinary video/audio/log callbacks still execute without the emulator lock.

## RTC

`ZxNextMachine::rtc()` is a DS1307-compatible clock at I2C address 68h, controlled
through SCL 103Bh and open-drain SDA 113Bh. It supports repeated START, ACK/NACK,
sequential register access, BCD date/time, leap dates, 12/24-hour format, the
clock-halt bit and 56 bytes of battery RAM. Machine reset releases the bus and
preserves the clock and RAM. The unused square-wave output is not simulated.

`setTimeSource()` accepts calendar seconds for deterministic tests or host local
time; the default uses UTC system time. `nvram()`/`setNvram()` let a consumer
persist battery RAM independently of save states. Spectacol supplies host local
time and persists RAM in application settings.

## Keyboard and NMI

`Emulator::setNextKeyState(NextKey, pressed)` supplies the 16 dedicated compound
keys, separately from `ZxKey` and the classic keyboard matrix. NextREG B0/B1 read
their active-high state. Unless NR68 bit 4 disables it, those keys also contribute
the documented CS/SS chords to the matrix. Releasing one extended key cannot
release a separately held matrix key. Host input is not rewound with machine
state.

`triggerNextNmi(multiface)` presses the Multiface or DivMMC drive button. NR06
button enables, device availability and NMI ownership apply. Software NR02
requests expose their cause flags. The Multiface loads optional `enNextMf.rom`
from the Next SD image, maps its ROM/RAM on the 0066 fetch, handles its configured
paging ports and releases on RETN. Its entry watcher exists only while NMI is
pending, so it adds no instruction hook to normal execution.

## DACs

All four DAC channels and their port aliases are supported, including decode
register enables, Soundrive mode-2 precedence at FB, and F1/F9 precedence over
AY aliases. SpecDrum DF drives A+D. NextREG 2C/2D/2E write B/A+D/C; reads are
silent Pi I2S input (UART bridging does not emulate Pi I2S). Samples enter the
existing stereo mixer at their emulated event times.

## Verification

`test_next_devices` tests UART timing/FIFO boundaries and state, I2C transactions,
RTC dates/halt/NVRAM, keyboard matrix suppression, DAC decoding, NMI entry,
I/O decoder invalidation and Engine thread lifecycle.
The complete unit suite must pass in both Debug and Release with assertions.
Performance comparison artifacts and generated media belong under the build tree.
