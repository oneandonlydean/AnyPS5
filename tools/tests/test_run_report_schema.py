import copy
import json
from importlib import metadata
from pathlib import Path
import unittest

import jsonschema

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads((ROOT / "docs/run-report.schema.json").read_text(encoding="utf-8"))
FIXTURES = sorted((ROOT / "docs/run-report-fixtures").glob("*.json"))
EXPECTED_FIXTURES = {"stop-none", "stop-missing-function", "stop-unsupported-shader", "stop-unsupported-format", "stop-crash",
                     "stop-assert", "stop-other", "killed-recovered", "zero-present", "missing-metadata-gpu", "truncated"}
MAX_BYTES = 65536
KEYWORDS = {"$schema", "$ref", "$defs", "title", "description", "type", "const", "enum", "properties", "required",
            "additionalProperties", "items", "maxItems", "minItems", "minLength", "maxLength", "pattern", "minimum", "maximum",
            "exclusiveMinimum", "maxProperties", "allOf", "if", "then"}


def load(name):
    return json.loads((ROOT / "docs/run-report-fixtures" / f"{name}.json").read_text(encoding="utf-8"))


def schema_keywords(node, path="schema"):
    if isinstance(node, list):
        for index, item in enumerate(node):
            yield from schema_keywords(item, f"{path}[{index}]")
    elif isinstance(node, dict):
        for key, value in node.items():
            if key in ("properties", "$defs"):
                for name, child in value.items():
                    yield from schema_keywords(child, f"{path}.{key}.{name}")
            elif key in ("const", "enum", "required", "pattern", "description", "title", "$ref", "$schema", "type"):
                yield path, key
            else:
                yield path, key
                yield from schema_keywords(value, f"{path}.{key}")


class PinnedValidatorTests(unittest.TestCase):
    def test_validator_matches_the_pinned_requirement(self):
        pinned = {}
        for line in (ROOT / "tools/requirements-run-report.txt").read_text(encoding="utf-8").splitlines():
            name, _, version = line.partition("==")
            pinned[name] = version
        self.assertEqual(metadata.version("jsonschema"), pinned["jsonschema"])

    def test_schema_is_a_valid_draft_2020_12_schema(self):
        self.assertEqual(SCHEMA["$schema"], "https://json-schema.org/draft/2020-12/schema")
        jsonschema.Draft202012Validator.check_schema(SCHEMA)

    def test_schema_uses_only_plain_keywords_and_no_format(self):
        used = {key for _, key in schema_keywords(SCHEMA)}
        self.assertEqual(used - KEYWORDS, set())
        self.assertNotIn("format", used)


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.validator = jsonschema.Draft202012Validator(SCHEMA)

    def errors(self, document):
        return list(self.validator.iter_errors(document))

    def assertRejected(self, document):
        self.assertTrue(self.errors(document), "the document was accepted")


class FixtureTests(ReportTests):
    def test_the_expected_fixtures_exist(self):
        self.assertEqual({file.stem for file in FIXTURES}, EXPECTED_FIXTURES)

    def test_every_fixture_validates_and_fits_the_size_limit(self):
        self.assertGreater(len(FIXTURES), 0)
        for file in FIXTURES:
            with self.subTest(fixture=file.name):
                data = file.read_bytes()
                self.assertLess(len(data), MAX_BYTES)
                self.assertEqual(self.errors(json.loads(data.decode("utf-8"))), [])
                self.assertEqual(data.decode("utf-8"), json.dumps(json.loads(data), indent=2, ensure_ascii=False) + "\n")

    def test_every_stop_kind_has_a_fixture(self):
        kinds = {json.loads(file.read_text(encoding="utf-8"))["stop"]["kind"] for file in FIXTURES}
        self.assertEqual(kinds, set(SCHEMA["properties"]["stop"]["properties"]["kind"]["enum"]))

    def test_run_ids_are_unique(self):
        ids = [json.loads(file.read_text(encoding="utf-8"))["run"]["id"] for file in FIXTURES]
        self.assertEqual(len(ids), len(set(ids)))

    def test_milestones_stage_and_frames_are_consistent(self):
        order = ("loaded", "started", "firstDraw", "firstPresent")
        stages = {"loaded": "loads", "started": "starts", "firstDraw": "first-draw", "firstPresent": "presents"}
        for file in FIXTURES:
            with self.subTest(fixture=file.name):
                run = json.loads(file.read_text(encoding="utf-8"))["run"]
                reached = [run["milestones"][name] for name in order if name in run["milestones"]]
                self.assertEqual(reached, sorted(reached))
                if reached:
                    self.assertLessEqual(reached[-1], run["seconds"])
                furthest = [stages[name] for name in order if name in run["milestones"]]
                self.assertEqual(run["stage"], furthest[-1] if furthest else "unknown")
                self.assertEqual("firstPresent" in run["milestones"], run["framesPresented"] > 0)
                if run["framesPresented"] >= 2 and not run["recovered"]:
                    self.assertAlmostEqual(run["averageFps"], (run["framesPresented"] - 1) / run["fpsWindowSeconds"], delta=run["averageFps"] * 0.02)

    def test_lists_are_sorted_by_count_highest_first(self):
        for file in FIXTURES:
            document = json.loads(file.read_text(encoding="utf-8"))
            for name, entries in (("functions", document["missing"]["functions"]), ("unsupported", document["shaders"]["unsupported"]),
                                  ("imageFormats", document["shaders"]["imageFormats"]), ("descriptors", document["shaders"]["descriptors"])):
                with self.subTest(fixture=file.name, list=name):
                    counts = [entry["calls" if name == "functions" else "count"] for entry in entries]
                    self.assertEqual(counts, sorted(counts, reverse=True))

    def test_the_truncated_fixture_reports_dropped_entries(self):
        document = load("truncated")
        self.assertEqual(len(document["missing"]["functions"]), 200)
        self.assertEqual(len(document["shaders"]["imageFormats"]), 100)
        self.assertTrue(all(document["truncated"][name] > 0 for name in ("functions", "unsupported", "imageFormats")))

    def test_the_recovered_fixture_is_marked_incomplete_and_killed(self):
        document = load("killed-recovered")
        self.assertEqual((document["run"]["recovered"], document["run"]["incomplete"], document["run"]["exit"]), (True, True, "killed"))
        self.assertEqual((document["stop"]["kind"], document["stop"]["reason"]), ("other", "unclean-unknown"))


class RejectionTests(ReportTests):
    def mutate(self, name, change):
        document = load(name)
        change(document)
        return document

    def test_a_missing_required_key_is_rejected_at_every_level(self):
        document = load("stop-missing-function")
        paths = [()]
        def walk(node, path):
            if isinstance(node, dict):
                for key, value in node.items():
                    paths.append(path + (key,))
                    walk(value, path + (key,))
        walk(document, ())
        required = []
        for path in paths[1:]:
            parent = document
            for key in path[:-1]:
                parent = parent[key]
            if path[-1] in SCHEMA_REQUIRED.get(path[:-1], ()):
                required.append(path)
        self.assertGreater(len(required), 40)
        for path in required:
            with self.subTest(path="/".join(path)):
                changed = copy.deepcopy(document)
                parent = changed
                for key in path[:-1]:
                    parent = parent[key]
                del parent[path[-1]]
                self.assertRejected(changed)

    def test_bad_enums_are_rejected(self):
        cases = [(("run", "exit"), "exploded"), (("run", "stage"), "converts"), (("stop", "kind"), "oops"), (("stop", "reason"), "window-closed"),
                 (("system", "os"), "freebsd"), (("system", "gpu", "vendor"), "Matrox"), (("shaders", "imageFormats", 0, "status"), "supported"),
                 (("shaders", "imageFormats", 0, "usage"), "texture"), (("shaders", "descriptors", 0, "usage"), "sampled"),
                 (("shaders", "descriptors", 0, "type"), "Texture"), (("coverage", "milestones"), "yes")]
        for path, value in cases:
            with self.subTest(path=path):
                document = load("stop-none")
                node = document
                for key in path[:-1]:
                    node = node[key]
                node[path[-1]] = value
                self.assertRejected(document)

    def test_lists_over_the_caps_are_rejected(self):
        document = load("stop-missing-function")
        entry = document["missing"]["functions"][0]
        document["missing"]["functions"] = [entry] * 201
        self.assertRejected(document)
        document = load("stop-unsupported-shader")
        document["shaders"]["unsupported"] = [document["shaders"]["unsupported"][0]] * 201
        self.assertRejected(document)
        document = load("stop-none")
        document["shaders"]["imageFormats"] = [document["shaders"]["imageFormats"][0]] * 101
        self.assertRejected(document)
        document = load("stop-none")
        document["shaders"]["descriptors"] = [document["shaders"]["descriptors"][0]] * 101
        self.assertRejected(document)
        document = load("stop-none")
        document["shaders"]["imageFormats"] = [document["shaders"]["imageFormats"][0]] * 100
        self.assertEqual(self.errors(document), [])

    def test_recovered_without_incomplete_is_rejected(self):
        document = load("killed-recovered")
        document["run"]["incomplete"] = False
        self.assertRejected(document)
        for path, value in ((("run", "exit"), "crash"), (("run", "exitCode"), 1), (("stop", "kind"), "crash"), (("stop", "reason"), "other")):
            with self.subTest(path=path):
                document = load("killed-recovered")
                document[path[0]][path[1]] = value
                self.assertRejected(document)
        document = load("stop-none")
        document["run"]["exit"] = "killed"
        self.assertRejected(document)

    def test_stop_kind_none_and_clean_exit_go_together(self):
        for change in (lambda d: d["run"].update(exit="crash"), lambda d: d["stop"].update(kind="crash"),
                       lambda d: d["stop"].update(reason="crash"), lambda d: d["stop"].update(kind="other")):
            document = load("stop-none")
            change(document)
            self.assertRejected(document)
        document = load("stop-crash")
        document["stop"].update(kind="none", reason="guest-exit")
        self.assertRejected(document)

    def test_reason_is_the_end_path_and_kind_is_what_stopped_the_run(self):
        rejected = [("stop-missing-function", dict(reason="guest-exit")), ("stop-missing-function", dict(reason="quit-requested")),
                    ("stop-unsupported-shader", dict(reason="guest-exit")), ("stop-assert", dict(reason="guest-exit")),
                    ("stop-crash", dict(reason="runtime-stop")), ("stop-assert", dict(reason="runtime-stop")),
                    ("stop-crash", dict(reason="other")), ("stop-assert", dict(reason="unclean-unknown")),
                    ("stop-none", dict(reason="runtime-stop")), ("stop-none", dict(reason="crash")),
                    ("stop-other", dict(reason="guest-exit")), ("stop-other", dict(reason="missing-function"))]
        for name, change in rejected:
            with self.subTest(fixture=name, change=change):
                document = load(name)
                document["stop"].update(change)
                self.assertRejected(document)
        accepted = [("stop-missing-function", dict(reason="crash")), ("stop-unsupported-shader", dict(reason="crash")),
                    ("stop-unsupported-format", dict(reason="other")), ("stop-other", dict(reason="runtime-stop")),
                    ("stop-other", dict(reason="crash")), ("stop-assert", dict(reason="crash")), ("stop-none", dict(reason="quit-requested"))]
        for name, change in accepted:
            with self.subTest(fixture=name, change=change):
                document = load(name)
                document["stop"].update(change)
                self.assertEqual(self.errors(document), [])

    def test_average_fps_needs_two_frames(self):
        document = load("stop-none")
        document["run"].update(framesPresented=1)
        self.assertRejected(document)
        document["run"].update(averageFps=None, fpsWindowSeconds=None)
        self.assertEqual(self.errors(document), [])

    def test_stage_needs_its_milestone(self):
        for stage, milestone in (("loads", "loaded"), ("starts", "started"), ("first-draw", "firstDraw"), ("presents", "firstPresent")):
            with self.subTest(stage=stage):
                document = load("stop-none")
                document["run"]["stage"] = stage
                del document["run"]["milestones"][milestone]
                self.assertRejected(document)
        document = load("stop-none")
        document["run"]["stage"] = "unknown"
        self.assertRejected(document)

    def test_a_section_with_coverage_off_cannot_list_entries(self):
        for flag, change in (("missing", lambda d: d["missing"].update(functions=[{"library": None, "name": "f", "calls": 1}])),
                             ("unsupported", lambda d: d["shaders"].update(unsupported=[{"encoding": None, "opcode": None, "opcodeValue": None, "modifiers": None, "count": 1}])),
                             ("imageFormats", lambda d: d["truncated"].update(imageFormats=1)),
                             ("descriptors", lambda d: None),
                             ("shaders", lambda d: d["shaders"].update(compiled=1)),
                             ("milestones", lambda d: None)):
            with self.subTest(flag=flag):
                document = load("stop-none")
                document["coverage"][flag] = False
                change(document)
                if flag in ("descriptors", "milestones"):
                    self.assertRejected(document)
                    continue
                if flag == "imageFormats":
                    document["shaders"]["imageFormats"] = []
                if flag == "shaders":
                    document["shaders"]["compiled"] = 1
                self.assertRejected(document)

    def test_nulls_are_only_allowed_where_the_contract_says(self):
        for path in (("run", "seconds"), ("run", "id"), ("run", "framesPresented"), ("run", "stage"), ("stop", "message"), ("stop", "reason"),
                     ("shaders", "compiled"), ("missing", "functions"), ("coverage", "missing"), ("run", "recovered")):
            with self.subTest(path=path):
                document = load("stop-none")
                document[path[0]][path[1]] = None
                self.assertRejected(document)
        for path in (("anyps5", "commit"), ("anyps5", "release"), ("anyps5", "modified"), ("game", "titleId"), ("game", "contentId"), ("game", "contentVersion"),
                     ("game", "name"), ("system", "os"), ("system", "osVersion"), ("system", "cpu"), ("system", "ramGiB"), ("system", "gpu"),
                     ("run", "exitCode"), ("stop", "source")):
            with self.subTest(path=path):
                document = load("stop-none")
                document[path[0]][path[1]] = None
                self.assertEqual(self.errors(document), [])

    def test_malformed_values_are_rejected(self):
        cases = [(("run", "id"), "3F9A1C07B2D84E5F9A6C0D1E2B3A4F56"), (("run", "id"), "3f9a"), (("game", "titleId"), "ppsa12345"),
                 (("game", "titleId"), "PPSA1234"), (("game", "titleId"), "PPSA123456"), (("anyps5", "commit"), "abc123"),
                 (("generated",), "2026-10-29 12:00:00"), (("generated",), "2026-10-29T12:00:00+13:00"), (("version",), 2),
                 (("format",), "anyps5-run"), (("run", "seconds"), -1), (("run", "seconds"), float("inf")), (("run", "seconds"), 1e12),
                 (("run", "framesPresented"), 1.5), (("run", "exitCode"), 1.5), (("system", "ramGiB"), 0), (("system", "gpu", "vulkan"), "1.3"),
                 (("stop", "message"), "x" * 301), (("stop", "message"), ""), (("stop", "message"), "line one\nline two"),
                 (("game", "name"), "x" * 301), (("stop", "source"), "../secret.cpp:1"), (("stop", "source"), "/home/user/a.cpp:1"),
                 (("stop", "source"), "core/a/../b.cpp:1"), (("stop", "source"), "core/a.cpp"), (("stop", "source"), "core/a.cpp:12x")]
        for path, value in cases:
            with self.subTest(path=path, value=value):
                document = load("stop-none")
                node = document
                for key in path[:-1]:
                    node = node[key]
                node[path[-1]] = value
                self.assertRejected(document)

    def test_the_maximum_string_length_is_accepted(self):
        document = load("stop-none")
        document["stop"]["message"] = "é" * 300
        document["game"]["name"] = "x" * 300
        self.assertEqual(self.errors(document), [])

    def test_unknown_keys_are_rejected_everywhere(self):
        for path in ((), ("anyps5",), ("game",), ("system",), ("system", "gpu"), ("run",), ("run", "milestones"), ("coverage",), ("truncated",),
                     ("missing",), ("shaders",), ("stop",), ("missing", "functions", 0), ("shaders", "imageFormats", 0), ("shaders", "descriptors", 0)):
            with self.subTest(path=path):
                document = load("stop-missing-function" if path[:2] == ("missing", "functions") else "stop-none")
                node = document
                for key in path:
                    node = node[key]
                node["extra"] = 1
                self.assertRejected(document)

    def test_unknown_opcode_keeps_its_number_with_a_null_name(self):
        document = load("stop-unsupported-shader")
        entry = {"encoding": None, "opcode": None, "opcodeValue": 711, "modifiers": None, "count": 1}
        document["shaders"]["unsupported"] = [entry]
        self.assertEqual(self.errors(document), [])
        entry["opcodeValue"] = -1
        self.assertRejected(document)
        entry.update(opcodeValue=None, count=0)
        self.assertRejected(document)

    def test_milestone_keys_are_optional_but_typed(self):
        document = load("stop-none")
        document["run"]["milestones"]["firstDraw"] = "8.9"
        self.assertRejected(document)


SCHEMA_REQUIRED = {}


def collect_required(node, path=()):
    if isinstance(node, dict):
        if "required" in node and "properties" in node:
            SCHEMA_REQUIRED[path] = tuple(node["required"])
        for name, child in node.get("properties", {}).items():
            collect_required(child, path + (name,))


collect_required(SCHEMA)


if __name__ == "__main__":
    unittest.main()
