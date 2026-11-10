"""Memory-block failures are errors, not a quickened-counter exemption."""

import unittest
from ci_pipeline.jit311.refleak_verdict import check

SUCCESS = '''== Tests result: SUCCESS ==
1 test OK.
Total tests: run=12
Total test files: success=1
Result: SUCCESS
'''


class RefleakVerdictTests(unittest.TestCase):
    def test_original_success(self):
        self.assertTrue(check('test_binop', SUCCESS, 0)[0])

    def test_every_leak_kind_stays_red_even_with_zero_exit(self):
        for kind in ('references', 'memory blocks', 'file descriptors'):
            for rc in (0, 2):
                with self.subTest(kind=kind, rc=rc):
                    text = f'test_binop leaked [1, 1, 1] {kind}\n' + SUCCESS
                    self.assertFalse(check('test_binop', text, rc)[0])

    def test_crash_timeout_nonzero_and_missing_output_fail(self):
        for rc in (-11, 1, 2, 124):
            self.assertFalse(check('test_binop', SUCCESS, rc)[0])
        for text in ('', SUCCESS.replace('Result: SUCCESS', ''),
                     SUCCESS.replace('run=12', 'run=0'),
                     SUCCESS.replace('success=1', 'success=0 failed=1'),
                     SUCCESS + 'FAIL: real_error\n', SUCCESS + SUCCESS):
            self.assertFalse(check('test_binop', text, 0)[0])


if __name__ == '__main__':
    unittest.main()
