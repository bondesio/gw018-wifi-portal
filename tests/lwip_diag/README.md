# Diagnostic socket helpers against the actual SDK lwIP

Run from the workspace:

```bash
./gw018-dm-firmware/tests/lwip_diag/run.sh
```

The script compiles every SDK `src/core/*.c`, `src/core/ipv4/*.c`, and
`src/api/*.c` source with GCC AddressSanitizer and UndefinedBehaviorSanitizer.
It uses the production `sockets.c` directly, included once by the test translation
unit so the test can inspect the static socket table without a production API
or source modification. Set `LWIP_SDK_ROOT` to select another SDK checkout.
Build artifacts live in a temporary `/tmp` directory and are removed on exit.
Execution has a 45-second watchdog.

`host_sys.c` supplies pthread semaphores, mailboxes, locks and time to the real
TCPIP thread. The test preserves the target's `NO_SYS=0`, core locking disabled,
per-netconn completion semaphore and MPU settings, plus its relevant TCP and
pool limits. Host pointer alignment is 8 bytes. Test-only loopback support,
allocation statistics, pool overflow checks and pool sanity checks are enabled.
DHCP, DNS, ARP, Ethernet and IGMP are disabled because the isolated loopback
fixture does not need them. The platform-only FreeRTOS priority and PMU wake
hooks are no-ops in the host architecture header.

The checks establish real socket connections through lwIP's IPv4/TCP loopback:

- UDP and listener rejection preserve the socket for the caller.
- In 100 cycles, the sender PCB's advertised send window is set to zero on the
  TCPIP thread. A 511-byte nonblocking socket send must create an actual unsent
  TCP segment and pbuf. `lwip_diag_can_send()` must return zero repeatedly.
- `lwip_abortclose()` must return success, free the socket and reset its peer.
  The peer's PCB must become null through the real TCP reset callback, and that
  socket must also close successfully.
- Both descriptors must be reused on every subsequent cycle. Each cycle must
  restore every non-timer memory pool, lwIP heap use, semaphore count and mailbox
  count. PCB lists must have no residual active, bound or TIME_WAIT connection.
- A partial receive must retain a real `lastdata` pbuf; aborting with both that
  receive buffer and an unsent transmit buffer must release both.
- A deliberately busy netconn must reject both helpers while preserving
  ownership, and close successfully after the test restores its idle state.
- Invalid descriptors must fail with `EBADF`. Completion semaphore signals must
  never accumulate a duplicate token.

lwIP starts its TCP timer lazily on the first connection and stops it at the
next tick after the last abort. During cycles, the timer pool may have at most
one extra entry. After the last listener closes and 300 ms elapses, its count
must also return exactly to the initial count.

Passing this harness verifies actual lwIP cleanup and API synchronization under
its single-owner contract. It does not exercise the FreeRTOS scheduler, Wi-Fi
hardware/driver, concurrent ownership of one descriptor, ISR calls or firmware
startup. The fixed lwIP allocators remain in use; sanitizer checks are
complemented by lwIP pool guards, sanity checks and exact allocation accounting.
