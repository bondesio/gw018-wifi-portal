# Bridge health source integration tests

Run `tests/bridge_health/run.sh` from either project. The script compiles a
single host translation unit that directly includes that project's actual
`example_socket_tcp_trx_1.c`, with its real diagnostic header. No firmware
source is rewritten. The platform headers, UART/socket/semaphore/RTOS APIs,
and diagnostic storage are mocked; all I/O stays inside the process.

The host compiler uses AddressSanitizer, UndefinedBehaviorSanitizer and
`-Wall -Wextra -Werror`. Only the original bridge's signed loop comparisons
and unused task parameters are suppressed. The executable lives in a
temporary directory removed after the run. `-fno-pie -no-pie` avoids host
ASan address-layout instability.

Checks exercise:

- State published before UART/TCP semaphore acquisition, UART getc/putc,
  TCP send/recv, accept, production select, and child-task waiting.
- A mocked blocking operation escapes through `longjmp`; the caller checks
  its task state and exact successful-byte progress count. Mock time advances
  30 seconds while the recorded progress timestamp stays unchanged.
- Progress follows completed UART calls and positive TCP results, including
  partial send and the first completed UART byte before a second byte blocks.
  Failed/zero sends and receive EOF/errors/retries add no byte progress.
- TX, RX and bridge stop before `vTaskDelete(NULL)` on tested exit paths.
- A suspended RX UART write retains its held UART semaphore and state; a
  subsequent independent TX invocation stops at UART_SEM with no new TX
  progress. This models the health markers under hypothetical UART/CTS
  backpressure without claiming to sample physical CTS.
- Canonical all-interface binding and blocking accept remain distinct from
  production station-IP binding, readiness/IP waiting and select polling.
  Production polling reports ACCEPT without a client and WAIT_TASKS with one.

Limits: `longjmp` preserves observable mock state but is not an RTOS scheduler
or resumable coroutine. Real semaphore fairness, UART/CTS electrical behavior,
Wi-Fi timing, hardware stack depth and sustained target uptime require target
validation. This suite verifies bridge call-site ordering; it deliberately
mocks diagnostic storage, whose actual implementation has its own
`tests/gateway_diag` suite.
