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

    def test_usage_says_where_the_report_tests_pass(self):
        text = (ROOT / "docs/user/USAGE.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"pass on Linux and on Windows in the fork's CI \(run \d+\)")
        self.assertNotRegex(text, r"not supported on Windows yet")

    def test_docs_do_not_infer_not_reached_from_a_lower_stage(self):
        for name, path in DOCS.items():
            text = path.read_text(encoding="utf-8")
            self.assertNotRegex(text, r"Only a stage lower", name)
            self.assertNotRegex(text, r"is not one the game failed to reach", name)

    def test_privacy_is_described_as_a_filter(self):
        usage = (ROOT / "docs/user/USAGE.md").read_text(encoding="utf-8")
        template = (ROOT / ".github/DISCUSSION_TEMPLATE/game-bring-up.yml").read_text(encoding="utf-8")
        self.assertRegex(usage, r"a filter, not a guarantee")
        self.assertRegex(template, r"a filter and not a guarantee")
        self.assertNotRegex(usage + template, r"holds no user name")

    def test_recovery_is_documented_as_strict(self):
        text = (ROOT / "docs/user/run-report.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"dropped whole")

    def test_template_is_valid_yaml_when_a_parser_is_available(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML is not installed")
        document = yaml.safe_load((ROOT / ".github/DISCUSSION_TEMPLATE/game-bring-up.yml").read_text(encoding="utf-8"))
        self.assertIn("report", [block.get("id") for block in document["body"]])

    def test_template_tells_users_to_attach_a_large_report(self):
        text = (ROOT / ".github/DISCUSSION_TEMPLATE/game-bring-up.yml").read_text(encoding="utf-8")
        self.assertRegex(text, r"attach the file")

    def test_usage_says_where_each_report_lands_and_what_a_killed_run_says(self):
        text = (ROOT / "docs/user/USAGE.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"A run that was killed \(found at the next start next to the game\)")
        self.assertRegex(text, r"`reports/<run id>/anyps5-report.json` in the run folder, not next to the executable")
        self.assertIn("`kind: other`, `reason: unclean-unknown`", text)

    def test_system_text_rules_are_documented(self):
        text = (ROOT / "docs/user/run-report.md").read_text(encoding="utf-8")
        for phrase in ("a fixed list in the source", "`NAME` and `ID_LIKE` never count", "unmatched quote", "also clears `Intel Arc`", "long git hash", "A login is never given that allowance", "first three numbers of the kernel release", "UUID"):
            self.assertIn(phrase, text)

    def test_system_fields_are_documented(self):
        text = (ROOT / "docs/user/run-report.md").read_text(encoding="utf-8")
        for field in ("system.osVersion", "system.cpu", "system.ramGiB", "system.gpu"):
            self.assertIn(field, text)

    def test_usage_says_the_same(self):
        text = (ROOT / "docs/user/USAGE.md").read_text(encoding="utf-8")
        self.assertRegex(text, r"either one the run did not reach or one this system does not record")


class TemplateTests(unittest.TestCase):
    def test_bring_up_template_has_the_report_field(self):
        text = (ROOT / ".github/DISCUSSION_TEMPLATE/game-bring-up.yml").read_text(encoding="utf-8")
        self.assertRegex(text, r"(?m)^    id: report$")
        self.assertIn("anyps5-report.json", text)


if __name__ == "__main__":
    unittest.main()
