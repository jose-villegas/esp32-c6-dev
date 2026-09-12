import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = ROOT / "launcher" / "tools" / "gen_launcher_layout.py"
LAYOUT_PATH = ROOT / "launcher" / "main" / "ui" / "launcher_layout.json"

SPEC = importlib.util.spec_from_file_location("gen_launcher_layout", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class LauncherLayoutGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.layout = json.loads(LAYOUT_PATH.read_text(encoding="utf-8"))

    def assert_rejected(self, layout, message):
        with self.assertRaisesRegex(ValueError, message):
            GENERATOR.validate(layout)

    def test_checked_in_layout_is_valid(self):
        generated = GENERATOR.generate(self.layout)
        self.assertEqual(
            generated,
            (ROOT / "launcher" / "main" / "ui" / "launcher_layout_generated.h").read_text(
                encoding="utf-8"
            ),
        )

    def test_card_overlap_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["landscape"]["rects"]["library"] = [100, 98, 125, 173]
        self.assert_rejected(layout, "overlaps")

    def test_out_of_bounds_rect_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["portrait"]["rects"]["status_bar"] = [8, 8, 400, 34]
        self.assert_rejected(layout, "leaves the canvas")

    def test_page_indicator_above_cards_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["landscape"]["rects"]["page_indicator"] = [200, 260, 48, 8]
        self.assert_rejected(layout, "below every app card")

    def test_small_tap_target_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["portrait"]["rects"]["render_lab"] = [100, 250, 43, 150]
        self.assert_rejected(layout, "44px tap target")


if __name__ == "__main__":
    unittest.main()
