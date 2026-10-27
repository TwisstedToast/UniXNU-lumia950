import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location('probe_qemu', Path(__file__).with_name('test-probe-qemu.py'))
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)


class GuardTranscriptTests(unittest.TestCase):
    def test_mmio_success_does_not_truncate_scm_evidence(self):
        transcript = b'NATIVE MMIO GUARD PASSED\n'
        self.assertFalse(PROBE.guard_finished(transcript))
        transcript += b'NATIVE SCM GUARD PASSED\n'
        self.assertFalse(PROBE.guard_finished(transcript))
        for ending in (b'SCM_QUERY_RETURNED_UNSUPPORTED\n', b'SCM_UNDEFINED_TRAP_RECOVERED\n'):
            with self.subTest(ending=ending):
                self.assertTrue(PROBE.guard_finished(transcript + ending))

    def test_explicit_guard_failure_stops_without_waiting(self):
        for marker in (b'NATIVE MMIO GUARD FAILED', b'NATIVE SCM GUARD FAILED'):
            with self.subTest(marker=marker):
                self.assertTrue(PROBE.guard_finished(marker))

    def test_other_application_completion_is_not_guard_completion(self):
        self.assertFalse(PROBE.guard_finished(b'PROBE COMPLETE\nReturning to firmware.\n'))


if __name__ == '__main__':
    unittest.main()
