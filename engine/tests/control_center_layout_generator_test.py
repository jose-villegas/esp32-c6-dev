import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = ROOT / "launcher" / "tools" / "gen_control_center_layout.py"
LAYOUT_PATH = ROOT / "launcher" / "main" / "ui" / "control_center_layout.json"
GENERATED_PATH = ROOT / "launcher" / "main" / "ui" / "control_center_layout_generated.h"

SPEC = importlib.util.spec_from_file_location("gen_control_center_layout", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class ControlCenterLayoutGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.layout = json.loads(LAYOUT_PATH.read_text(encoding="utf-8"))

    def assert_rejected(self, layout, message):
        with self.assertRaisesRegex(ValueError, message):
            GENERATOR.validate(layout)

    def test_checked_in_layout_is_valid(self):
        self.assertEqual(
            GENERATOR.generate(self.layout),
            GENERATED_PATH.read_text(encoding="utf-8"),
        )

    def test_overlap_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["portrait"]["rects"]["bluetooth"] = [16, 54, 160, 78]
        self.assert_rejected(layout, "overlaps")

    def test_small_control_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["landscape"]["rects"]["volume"][3] = 43
        self.assert_rejected(layout, "44px tap target")


if __name__ == "__main__":
    unittest.main()
