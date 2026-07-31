# Graphics Device

## Implemented display path

LampVM keeps the firmware framebuffer for BIOS and early-kernel output, then
adds a PCI dumb-display endpoint at `00:02.0` (`1DB7:2000`, class `03:00:00`).
The current kernel enumerates the endpoint, validates its ABI, copies the
existing firmware scanout into PCI VRAM, and publishes that image with a page
flip. Only after that succeeds does the framebuffer console switch its writes
to VRAM. If the endpoint is absent or validation/submission fails, the console
continues using the firmware framebuffer.

The fixed mode is `640x480`, stride `2560`, XRGB8888. The device exposes:

| BAR | Size | Type | Purpose |
|---|---:|---|---|
| BAR0 | 4 KiB | 32-bit memory | Control/status registers |
| BAR1 | 4 MiB | 32-bit prefetchable memory | Guest-visible VRAM |

The ABI constants and register offsets live in
`include/lampvm/device_abi.h`, shared by the host and guest kernel.

## Control ABI v1

BAR0 reports magic `LPG1`, ABI version 1, capabilities, status, geometry,
format, and VRAM size. The mutable state consists of:

- current and pending scanout offsets;
- a damage rectangle (`x`, `y`, `width`, `height`);
- command submission/completion sequence numbers;
- interrupt status, enable, and write-one-to-ack registers;
- cursor X/Y and control registers for the fixed 12x18 cursor plane.

Commands are `ENABLE`, `DISABLE`, `FLUSH`, `PAGE_FLIP`, `CURSOR_UPDATE`, and
`RESET`.
`FLUSH` publishes the requested damage rectangle from the current VRAM
scanout. `PAGE_FLIP` validates the pending offset, makes it current, and
publishes the whole frame. Page flips complete synchronously in v1 and may
raise MSI; this is a completion interrupt, not a modeled vertical blank.

`CURSOR_UPDATE` applies position, visibility, and button-state changes without
publishing or recomposing a scanout buffer. The host restores the old cursor
footprint from current VRAM and overlays the cursor at its new position under
framebuffer row locks. Consequently SDL and VNC observe the same cursor plane,
while the guest compositor continues to own only the underlying scene.

A `640x480x4` frame consumes 1,228,800 bytes. The 4 MiB BAR holds two complete
scanout buffers at offsets `0` and `0x12C000`, with roughly 1.65 MiB left for
future surfaces. The v1 capability advertises `SHADOW_SCANOUT`: BAR1 is guest
VRAM, while the host's existing framebuffer is the presentation surface.
Flush/page-flip atomically copy damage or a frame into that surface. Any
4-byte-aligned scanout offset for which a complete frame fits inside BAR1 is
valid.

## Kernel takeover and fallback

`kernel/src/gpu.c` performs PCI driver validation and calls the framebuffer
console attach path. The console then:

- writes glyphs directly into BAR1;
- submits one 8x8 damage rectangle for ordinary glyph updates;
- performs software clear/scroll in VRAM and submits full-frame damage;
- enables MSI for page-flip completion and device errors, but not for every
  character flush.

On the host, the display device snapshots the firmware framebuffer before its
first enable. `DISABLE` or `RESET` restores that snapshot. The kernel also
detaches the PCI console after a device-error MSI, so the original scanout is
a runtime fallback as well as a probe-time fallback.

## Window manager and screen ownership

The framebuffer is a text console only during firmware and early kernel boot.
After PCI display, audio, and Ethernet probing, `kernel/src/graphics.c` hands
VM Display to the kernel window manager in `kernel/src/wm.c`. WM v0 provides:

- a fixed-size window table with visibility, position, z-order, title, accent,
  and client text lines;
- desktop, panel, window-frame, title-bar, focus, and shadow composition;
- `create`, `move`, `raise`, `show/hide`, and content-line kernel APIs;
- click-to-raise behavior for overlapping windows;
- two scanout buffers with full-scene composition into the back buffer followed
  by a PCI page flip;
- a PS/2 pointer backed by the PCI GPU cursor plane, with a software fallback.

Mouse IRQ has priority `0xD0`, above the timer (`0xC0`) and ordinary device
completion (`0xA0`). Its common movement path coalesces consecutive PS/2
packets and writes only the cursor registers; it never recomposes or page-flips
the desktop or takes the compositor's drawing lock. This models the latency
split used by real display stacks: input and cursor-plane updates remain
independent of the lower-frequency window compositor. If the cursor capability
is absent, a save-under fallback updates only the union of the old and new
cursor rectangles. Window mutations currently do a full back-buffer composition.

Runtime terminal traffic is deliberately separate from that screen:

- kernel logs and stdout/stderr are always written to the serial device;
- TTY input and local echo use the VM Serial window (or host terminal in
  headless/no-serial-window modes);
- PS/2 keyboard input from VM Display is reserved once graphics is active and
  is not injected into the shell;
- PS/2 three-byte mouse packets drive the WM pointer, while button 1 selects and
  raises the topmost hit-tested window;
- the panic path re-enables framebuffer text, clears the graphical screen, and
  displays the panic diagnostics there as well as on serial.

## SDL3 presentation

The final host presentation remains SDL3 on Linux and macOS. It uses a
streaming `ARGB8888` texture with blending disabled, a resizable logical
`640x480` viewport, nearest-neighbor scaling, and scanline dirty tracking.
Both firmware framebuffer writes and PCI GPU publication converge on this
same scanout path, so SDL and VNC do not need separate device-specific code.
Clicking VM Display enables SDL3 relative mouse mode, which hides and grabs the
host pointer while forwarding relative motion as PS/2 packets. Ctrl+Alt+G
releases it on Linux and Windows; Control+Command+G is the macOS equivalent.
This path is shared by Linux and macOS. Fractional SDL3 motion is carried
between frames before integer PS/2 encoding. On macOS, trackpads remain on
SDL's mouse path and relative motion uses the system acceleration curve; the
relative constraint covers the 2D window instead of forcing the pointer to its
center. The legacy PS/2 compatibility FIFO is independent of the active 8042
queue, so an unconsumed legacy interface cannot stop pointer delivery.

The host display loop uses one monotonic nanosecond deadline at 120 Hz. SDL
renderer VSync is disabled so it cannot introduce a second blocking clock;
missed deadlines are dropped instead of replayed as a burst. The display thread
and BSP VM thread request SDL's high scheduling priority, while additional VM
cores retain normal priority. This gives input delivery and scanout publication
a bounded frame budget without growing queues that would increase latency.

## Current limitations

- one fixed mode and pixel format;
- no true vblank timing, EDID, mode setting, or 2D/3D engine;
- the cursor plane has a fixed shape and hotspot rather than programmable cursor
  image memory;
- synchronous shadow publication rather than zero-copy host scanout;
- no userspace DRM/KMS-style ownership or mapping API;
- the compositor and window objects are kernel-owned; there is no userspace
  surface/event ABI yet;
- window mutations recompose a complete frame; pointer movement bypasses the
  compositor through the cursor plane;
- no pointer dragging, resizing, keyboard focus, or application event delivery;
- early-boot and panic console scroll still requires a software VRAM copy.

The PCI audio endpoint and reusable DMA completion-ring ABI are documented in
`docs/audio.md`.
