import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("graphics_profile", Path(__file__).with_name("check-graphics-profile.py"))
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def firmware_table():
    entries = []
    for index, rate in enumerate((600000, 490000, 450000, 366000, 300000, 180000, 0)):
        entries.append(f'Package(3) {{ "PSTATE", {index}, Package(2) {{ "REQUIRED_RESOURCE", '
                       f'Package(3) {{ One, "/clk/oxili", {hex(rate)} }} }} }}')
    entries.extend(f'Package(2) {{ "{name}", 4 }}' for name in
                   ("PRELOAD_PSTATE", "PREPARE_PSTATE", "ABANDON_PSTATE"))
    wake = 'Package(2) { "PSTATE_SET", 1, Package(3) { One, "/clk/oxili", 0x4b00 }, Package(3) { One, "/clk/oxili", Zero } }'
    return 'Package(12) { "PSTATE_SET", 2, ' + ",".join(entries) + " }" + wake


class GraphicsProfileTests(unittest.TestCase):
    def test_active_and_wake_are_not_interchangeable(self):
        result = profile.extract_profile(firmware_table())
        self.assertEqual(result["startup_khz"], 300000)
        self.assertEqual(result["wake_khz"], 19200)
        self.assertEqual(result["active_rates_khz"][3], 366000)

    def test_rate_policy_order_and_package_ambiguity_refused(self):
        original = firmware_table()
        mutations = [original.replace("0x493e0", "0x4b00"),
                     original.replace("0x595b0", "367000"),
                     original.replace('"PREPARE_PSTATE", 4', '"PREPARE_PSTATE", 5'),
                     original.replace('"PSTATE", 4', '"PSTATE", 5'),
                     original.replace("0x4b00", "27000"),
                     original + original,
                     original.replace("Package(12)", "Package(12) {")]
        for table in mutations:
            with self.subTest(table=table):
                with self.assertRaises(ValueError):
                    profile.extract_profile(table)

    def test_foreign_device_tables_do_not_supply_the_graphics_rate(self):
        foreign = firmware_table().replace("/clk/oxili", "/clk/other")
        self.assertEqual(profile.extract_profile(foreign + firmware_table())["startup_khz"], 300000)
        with self.assertRaises(ValueError):
            profile.extract_profile(foreign)


if __name__ == "__main__":
    unittest.main()
