"""The run report docs must say which milestones each system records, so that a missing loaded/started on Linux or Windows is not read as
"not reached", and the bring-up discussion template must have the report field."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DOCS = {"USAGE": ROOT / "docs/user/USAGE.md", "run-report": ROOT / "docs/user/run-report.md"}


def rows(text):
    table = {}
    for line in text.splitlines():
        cells = [cell.strip().strip("`") for cell in line.strip().strip("|").split("|")]
        if len(cells) == 4 and cells[0] in ("Linux", "Windows", "macOS"):
            table[cells[0]] = cells[1:]
    return table


class MilestoneDocsTests(unittest.TestCase):
    def test_every_system_has_a_row_in_both_docs(self):
        for name, path in DOCS.items():
            table = rows(path.read_text(encoding="utf-8"))
            self.assertEqual(sorted(table), ["Linux", "Windows", "macOS"], f"{name} lacks a system row")

    def test_loaded_and_started_are_macos_only(self):
        for name, path in DOCS.items():
            table = rows(path.read_text(encoding="utf-8"))
            self.assertTrue(table["Linux"][0].endswith("recorded") and table["Linux"][0].startswith(("not", "never")), name)
            self.assertTrue(table["Windows"][0].startswith(("not", "never")), name)
            self.assertEqual(table["macOS"][0], "recorded", name)
            for system in table:
                self.assertEqual(table[system][1], "recorded", f"{name}: {system} firstDraw/firstPresent")
                self.assertEqual(table[system][2], "recorded", f"{name}: {system} stage and frames")

    def test_a_missing_milestone_is_not_presented_as_not_reached(self):
        text = (ROOT / "docs/user/run-report.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"never as .not reached.")
        self.assertIn("coverage.milestones", text)

    def test_usage_says_the_same(self):
        text = (ROOT / "docs/user/USAGE.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"is not one the game failed to reach")


class TemplateTests(unittest.TestCase):
    def test_bring_up_template_has_the_report_field(self):
        text = (ROOT / ".github/DISCUSSION_TEMPLATE/game-bring-up.yml").read_text(encoding="utf-8")
        self.assertRegex(text, r"(?m)^    id: report$")
        self.assertIn("anyps5-report.json", text)


if __name__ == "__main__":
    unittest.main()
