# EHCI ISO regression tests

Run `python test/host/ehci/run.py` from the repository root with GCC on PATH
(or set `CC`). On Windows use a MinGW compiler with its runtime DLLs on PATH.

The fixture includes the actual driver, supplies static DMA memory below
4 GiB, and simulates descriptor writeback and FRINDEX. On 64-bit hosts only
the QH software pointer tail differs; hardware descriptor layouts are checked
explicitly. Target builds retain the original 32-bit ABI assertions.

Coverage includes native FS descriptors, external TT routing/masks, HS
high-bandwidth descriptors and page crossing, short/zero/error completion,
exactly-once completion, limits/pool exhaustion, deferred arming/cancellation,
all interval encodings and ring slots, counter wrap, late completion, and the
inactive QH overlay/new active qTD regression found during audio streaming.
Queue tests verify distinct DMA descriptors/buffers, consecutive HS service
slots, full-queue rejection, FIFO retirement and slot reuse. The runner covers
ISO disabled, queue depth one, and queue depth two.

This fixture does not emulate DMA, cache coherency, periodic schedule
handshakes, or USB transactions. Hardware captures are required for those.
