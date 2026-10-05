#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

MODULE = Path(__file__).resolve().parents[2] / 'deploy' / 'haos-addon' / 'gw018_wifi_log' / 'collect_wifi_logs.py'
spec = importlib.util.spec_from_file_location('collector', MODULE)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class Records(unittest.TestCase):
    def test_headers(self):
        for version in (2, 3):
            self.assertTrue(collector.valid_record(
                f'gateway_diag version={version} port=81 tick_hz=1000 history=32 early=8 '
                'scope=application netif_bits=sta_up:1,sta_link:2,sta_ip:4,ap_up:8,ap_link:16,ap_ip:32'))

    def test_numeric_bounds(self):
        self.assertTrue(collector.valid_record('memory tick=4294967295 free_heap=0 min_heap=4294967295'))
        for invalid in ('-1', '4294967296', '1.0', 'x'):
            self.assertFalse(collector.valid_record(f'memory tick=0 free_heap={invalid} min_heap=0'))

    def test_health(self):
        for role in collector.ROLES:
            for state in collector.STATES:
                self.assertTrue(collector.valid_record(
                    f'health tick=123 role={role} alive=1 state={state} state_since=10 '
                    'heartbeat=120 last_progress=100 stack_free_bytes=256 generation=1'))
        record = ('health tick=1 role=tx alive=1 state=tcp_send state_since=0 '
                  'heartbeat=1 last_progress=0 stack_free_bytes=512 generation=1')
        for bad in (record.replace('role=tx', 'role=untrusted'),
                    record.replace('state=tcp_send', 'state=untrusted'),
                    record.replace('alive=1', 'alive=2'), record + ' extra=1',
                    record + ' tick=2', record.replace(' generation=1', '')):
            self.assertFalse(collector.valid_record(bad))

    def test_reject_arbitrary(self):
        for line in ('sdk console text', 'a' * 512, 'memory tick=1 free_heap=10 min_heap=0 extra=x',
                     'event seq=0 tick=0 name=untrusted value=1 lost=0'):
            self.assertFalse(collector.valid_record(line))


if __name__ == '__main__':
    unittest.main()
