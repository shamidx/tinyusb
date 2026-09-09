# USBH/audio queue regression tests

Run `python test/host/queue/run.py` with GCC on PATH (or set `CC`). Windows
requires MinGW and its runtime DLLs on PATH.

The fixture includes the actual USBH and audio drivers with a stub HCD. It
builds with the default depth of one and explicitly with depth two. The default
tests exercise terminal callback dispatch, busy rejection, submission failure,
abort, capture rearming, playback underrun silence, and stopped-stream draining.
The depth-two tests check nonterminal QUEUED ownership, full-queue handling, canceled claims,
claims surviving delayed events, failed-submission rollback, endpoint close
and reopen, FIFO audio buffer consumption, stopped-stream draining, restart,
and refill failure with a sibling transfer still pending. Playback checks
keep the sibling buffer intact while recycling the head, and explicit
feedback ignores QUEUED without allocating a second request. Public claims
reject queue credit atomically; application callbacks receive only terminal
events through the actual USBH task dispatcher.

This is a deterministic state-transition test; it does not emulate DMA or
prove RTOS scheduling latency. EHCI descriptor tests and hardware capture
provide separate coverage.
