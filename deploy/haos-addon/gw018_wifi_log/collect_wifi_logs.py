#!/usr/bin/env python3
"""Record GW018 port-81 application diagnostics with timestamps and reconnects.

Bounded rotating private files; accepts only structured version-2/3 records.
Example: collect_wifi_logs.py GATEWAY_IP --output ./gateway-debug.log
"""
import argparse
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import re
import socket
import time

EVENTS = frozenset('app_start network_start uart_ready bridge_start bridge_listen '
                  'bridge_connect bridge_disconnect bridge_error tx_exit rx_exit '
                  'task_error network_state log_listen log_error network_ready'.split())
STATS = frozenset('tick uart_rx tcp_tx tcp_rx uart_tx send_errors recv_errors '
                 'short_sends uart_buffer_drops bridge_connections bridge_disconnects '
                 'log_connections log_disconnects log_errors log_lost'.split())


ROLES = frozenset(('tx', 'rx', 'bridge', 'diag'))
STATES = frozenset('idle uart_sem uart_read uart_write tcp_sem tcp_send tcp_recv '
                   'accept wait_tasks network_wait delay stopped tcpip'.split())
RECORD_FIELDS = {
    'event': frozenset(('seq', 'tick', 'name', 'value', 'lost')),
    'stats': STATS,
    'memory': frozenset(('tick', 'free_heap', 'min_heap')),
    'health': frozenset(('tick', 'role', 'alive', 'state', 'state_since',
                         'heartbeat', 'last_progress', 'stack_free_bytes', 'generation')),
}
TEXT_FIELDS = {'event': {'name': EVENTS},
               'health': {'role': ROLES, 'state': STATES}}


def valid_record(line):
    if len(line) > 511 or not line.isascii():
        return False
    fields = line.split()
    if not fields:
        return False
    if fields[0] == 'gateway_diag':
        return bool(re.fullmatch(
            r'gateway_diag version=[23] port=81 tick_hz=[0-9]+ history=32 early=8 '
            r'scope=application netif_bits=sta_up:1,sta_link:2,sta_ip:4,'
            r'ap_up:8,ap_link:16,ap_ip:32', line))
    kind = fields[0]
    if kind not in RECORD_FIELDS:
        return False
    values = {}
    for field in fields[1:]:
        if '=' not in field:
            return False
        name, value = field.split('=', 1)
        if name in values or name not in RECORD_FIELDS[kind]:
            return False
        values[name] = value
        allowed = TEXT_FIELDS.get(kind, {}).get(name)
        if allowed is not None:
            if value not in allowed:
                return False
        elif not value.isdigit() or len(value) > 10 or int(value) > 4294967295:
            return False
    if kind == 'health' and values.get('alive') not in ('0', '1'):
        return False
    return values.keys() == RECORD_FIELDS[kind]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--port', type=int, default=81)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--duration', type=float, default=0,
                        help='Seconds; 0 records until interrupted')
    parser.add_argument('--idle-timeout', type=float, default=20,
                        help='Reconnect if no records arrive for this many seconds')
    args = parser.parse_args()
    if args.idle_timeout <= 0:
        parser.error('--idle-timeout must be positive')
    os.umask(0o077)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log = logging.getLogger('gateway-diag')
    log.setLevel(logging.INFO)
    handler = RotatingFileHandler(args.output, maxBytes=5 * 1024 * 1024,
                                  backupCount=4, encoding='ascii')
    os.chmod(args.output, 0o600)
    handler.setFormatter(logging.Formatter('%(asctime)sZ %(message)s'))
    handler.formatter.converter = time.gmtime
    log.addHandler(handler)
    end = time.monotonic() + args.duration if args.duration > 0 else float('inf')
    try:
        while time.monotonic() < end:
            try:
                with socket.create_connection((args.host, args.port), timeout=3) as peer:
                    peer.settimeout(1)
                    log.info('collector connected')
                    pending = b''
                    last_data = time.monotonic()
                    while time.monotonic() < end:
                        try:
                            data = peer.recv(1024)
                        except socket.timeout:
                            if time.monotonic() - last_data >= args.idle_timeout:
                                log.info('collector stream stalled')
                                break
                            continue
                        if not data:
                            break
                        last_data = time.monotonic()
                        pending += data
                        while b'\n' in pending:
                            raw, pending = pending.split(b'\n', 1)
                            line = raw.decode('ascii', errors='replace')
                            if valid_record(line):
                                log.info('%s', line)
                            else:
                                log.info('collector rejected invalid record')
                        if len(pending) > 511:
                            raise ValueError('Oversized record')
                    log.info('collector disconnected')
            except OSError as error:
                # Numeric OS errors add evidence without storing exception text.
                log.info('collector connection unavailable errno=%d',
                         error.errno if isinstance(error.errno, int) else 0)
            except ValueError:
                log.info('collector oversized record')
            remaining = end - time.monotonic()
            if remaining > 0:
                time.sleep(min(1, remaining))
    except KeyboardInterrupt:
        log.info('collector stopped')
    finally:
        handler.close()


if __name__ == '__main__':
    main()
