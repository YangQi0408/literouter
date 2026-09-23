#!/usr/bin/env python3
"""Check docs/{zh,en}/configuration.md against the config a build actually produces.

The sample JSON, the field tables and the code are three copies of the same
facts, and the copy that is not compiled is the one that drifts: `log_capacity`
was documented as 400 while the code defaulted to 200 (`ServerConfig` and
`test_config` both said so), and a macOS config path was invented
that no branch of the code produces. Reviewing that by eye does not scale.

So this walks the documentation against the code and reports what disagrees:

  * the sample JSON must name exactly the keys the binary emits for `server`
    (both directions: nothing missing, nothing invented) and carry exactly its
    defaults, since defaults are what that sample is for;
  * the provider, route and target objects in the sample must not lose a key the
    binary emits (one direction only: a field that is empty in the example, such
    as a route target's `model`, is still documented in its table);
  * every field the binary emits for `server`, `providers`, `routes` and a route
    target must be documented in its section — table row or bullet list, both
    count;
  * a documented default is compared with the code's, but only when the cell is
    a literal. Cells written in prose ("required", "依协议") are counted and
    reported rather than checked: they cannot be verified, and pretending
    otherwise would make this tool lie in the other direction.

Run it as CI does:

  bin="$(ls -t cli/target/*/*/bin/literouter | head -1)"   # newest build; see notes
  tmp="$(mktemp -d)"
  LITEROUTER_CONFIG="$tmp/seed.json" "$bin" config init --force
  printf '{"providers":[{}],"routes":[{"targets":[{"provider":"p"}]}]}' > "$tmp/minimal.json"
  # --force: the minimal config is deliberately invalid (an empty relay id), and
  # printing the effective config must not depend on validation passing.
  LITEROUTER_CONFIG="$tmp/minimal.json" "$bin" serve --print-config --force > "$tmp/defaults.json"
  python3 scripts/check_config_docs.py --seed "$tmp/seed.json" --defaults "$tmp/defaults.json"

`ls -t` because a checkout that has been rebuilt a few times keeps one directory
per source fingerprint under `cli/target/<triple>/`, and an older one names an
older binary — `mcpp clean --stale` drops the ones no build still uses.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# A section is recognised by the single backticked name in its heading, e.g.
# "### Server 配置 (`server`)" or "### Server Configuration (`server`)".
SECTION_HEADING = re.compile(r"^#{2,4}\s+(.*)$")
SECTION_NAME = re.compile(r"`([A-Za-z_]+)`")
# | `field` | `type` | `default` | description |
TABLE_ROW = re.compile(r"^\|\s*`([^`]+)`\s*\|([^|]*)\|([^|]*)\|")
# - `field` (`string`, 默认 `true`)：description
BULLET_FIELD = re.compile(r"^\s*-\s*`([A-Za-z_]+)`\s*\(([^)]*)\)")
BULLET_DEFAULT = re.compile(r"(?:默认|default)s?[^`]*`([^`]+)`", re.IGNORECASE)
# The sections whose fields this knows how to resolve. A route's target fields
# are documented inside the routes section, as nested bullets.
KNOWN_SECTIONS = ("server", "providers", "routes")

# A default cell that is not a literal has to be one of a closed set of claims
# this script knows how to check. Counting such cells instead — which is what it
# used to do — reports them and moves on, so a new prose cell nobody taught the
# checker about passes silently, and a cell that stops being true keeps passing
# forever. Anything outside this vocabulary is now an error rather than a note.
REQUIRED_WORDS = ("required", "必填", "必须")
BY_PROTOCOL_WORDS = ("by protocol", "by proto", "依协议")
# "by protocol (openai, anthropic, ...)" — the list is what makes the claim
# checkable: adding a protocol without extending this list is exactly the drift
# this catches.
PROTOCOL_LIST = re.compile(r"[（(]([^）)]*)[）)]")
# `other_field` — a default that falls back to a sibling field's value.
FIELD_REFERENCE = re.compile(r"^`([A-Za-z_]+)`$")


def classify_prose(cell: str | None) -> tuple[str, object]:
    """(form, argument) for a non-literal default cell.

    Forms: "empty", "required", "by_protocol", "reference", "unknown".
    """
    if cell is None or not cell.strip():
        return "empty", None
    text = cell.strip()
    lowered = text.lower()
    bare = text.replace("`", "").strip().lower()
    if bare in REQUIRED_WORDS:
        return "required", None
    if (match := FIELD_REFERENCE.match(text)) is not None:
        return "reference", match.group(1)
    for word in BY_PROTOCOL_WORDS:
        if lowered.startswith(word):
            names = PROTOCOL_LIST.search(text)
            if names is None:
                return "by_protocol", None
            return "by_protocol", [n.strip().strip("`") for n in names.group(1).split(",")
                                   if n.strip()]
    return "unknown", None


def protocols_documented(body: str) -> set[str]:
    """Every protocol name the `protocol` row's description enumerates.

    The description is the fourth column, not the third: the third is the
    default cell, and reading that one would find a single name and report every
    other protocol as undocumented.
    """
    for line in body.splitlines():
        row = TABLE_ROW.match(line)
        if row is None or row.group(1) != "protocol":
            continue
        columns = line.split("|")
        if len(columns) < 5:
            return set()
        return {name.strip().strip("`")
                for name in re.findall(r"`([A-Za-z_]+)`", columns[4])}
    return set()


def required_paths(report: dict) -> set[str]:
    """Field paths `config validate` reported as **errors**, from its --json report.

    The report is a list of issues, each carrying the locatable path the
    validator built (``providers[0].base_url``) and a level. Only `error` counts
    as a requirement: `info` and `warning` are advice about a config the
    validator accepts, so treating them as "this field is required" would flag
    rows that are correct — `providers.models` is reported as `info` when empty
    (the relay is still reachable through a route that names it) and is
    documented as defaulting to `[]`, which is true.
    """
    return {issue.get("path", "") for issue in report.get("issues", [])
            if issue.get("level") == "error"}


# How a documented section maps onto the validator's path roots.
SECTION_PATH_ROOTS = {
    "server": "server",
    "providers": "providers",
    "routes": "routes",
}


def reported_missing(paths: set[str], section: str, name: str) -> bool:
    """True when some reported error path points at this field of this section.

    Paths carry indices for array sections (``providers[0].id``), and a
    documented field may sit on the array element rather than on the object, so
    the match is on the section's path root and the trailing field name rather
    than on string equality.
    """
    root = SECTION_PATH_ROOTS.get(section, section)
    if f"{root}.{name}" in paths:
        return True
    return any(path.startswith(f"{root}[") and path.endswith(f"].{name}") for path in paths)


# Fields the validator rejects only *under a condition*, so a literal default in
# the table is the correct row rather than a mistake — and the reverse check
# ("a literal for a field validate() rejects") has to skip them or it reports
# false positives against two rows that are right:
#
#   server.api_key             required only when the listener is off-loopback
#
# Anything added here should be a condition the validator genuinely makes, not
# an exemption granted to silence a finding.
CONDITIONALLY_REQUIRED = {
    "server.api_key",
}


def unconditionally_required(paths: set[str], section: str, name: str) -> bool:
    """True when every reported path for this field is an unconditional error."""
    root = SECTION_PATH_ROOTS.get(section, section)
    matched = {path for path in paths
               if path == f"{root}.{name}"
               or (path.startswith(f"{root}[") and path.endswith(f"].{name}"))}
    if not matched:
        return False
    return not (matched & CONDITIONALLY_REQUIRED)


def check_prose(form: str, argument: object, section: str, name: str, cell: str | None,
                fields: dict, body: str, lang: str, problems: list[str],
                required_paths_seen: set[str] | None = None) -> str:
    """Checks a prose cell's claim.

    Returns "checked" when the claim was verified against the build's own output,
    "recognised" when it is a known claim this checker has no data for, and
    "unknown" when it is not a claim at all — which is an error, because a prose
    cell nobody taught the checker about is one that can stop being true without
    anyone noticing.
    """
    if form == "unknown":
        problems.append(
            f"{lang}: `{section}.{name}` documents default {cell!r}, which is neither a "
            f"literal nor one of the forms this checker understands "
            f"({'/'.join(REQUIRED_WORDS)}, {'/'.join(BY_PROTOCOL_WORDS)}, "
            f"`field`, or an empty cell) — teach the checker the claim or write a literal")
        return "unknown"
    if form in ("empty", "required"):
        # "Required" and "no default" are the same claim in these tables: there
        # is nothing to fall back on. Whether the field really is required is
        # validate()'s answer — so when its report is available the claim is
        # checked against it, and without the report it stays recognised rather
        # than being counted silently ("0 unknown cells" must not read as "all
        # verified").
        if form == "required" and required_paths_seen is not None:
            if not reported_missing(required_paths_seen, section, name):
                problems.append(
                    f"{lang}: `{section}.{name}` is documented as {cell!r}, but `config "
                    f"validate` does not report it when the field is omitted — either the "
                    f"validator stopped requiring it or the row is wrong")
            return "checked"
        return "recognised"
    if form == "reference":
        target = str(argument)
        if target not in fields:
            problems.append(
                f"{lang}: `{section}.{name}` documents a fallback to `{target}`, "
                f"which is not a field of `{section}`")
        return "checked"
    if form == "by_protocol":
        if argument is None:
            problems.append(
                f"{lang}: `{section}.{name}` says its default is per protocol but names none; "
                f"write `{'/'.join(BY_PROTOCOL_WORDS[:1])} (openai, anthropic, ...)` so that "
                f"adding a protocol without updating this row is caught")
            return "checked"
        documented = protocols_documented(body)
        # Every protocol the `protocol` row documents has to be accounted for
        # here: a protocol added to that row without a mention in this one is
        # exactly the drift this check exists to catch.
        unlisted = sorted(documented - set(argument))
        if unlisted:
            problems.append(
                f"{lang}: `{section}.{name}` says its default is per protocol but does not "
                f"mention {unlisted}, which the `protocol` row lists")
        unknown = sorted(set(argument) - documented)
        if unknown:
            problems.append(
                f"{lang}: `{section}.{name}` names {unknown}, which the `protocol` row does "
                f"not list")
        return "checked"
    return "unknown"


def strip_jsonc(text: str) -> str:
    """Removes // comments, leaving string contents alone (a URL keeps its //)."""
    out: list[str] = []
    index = 0
    in_string = False
    while index < len(text):
        char = text[index]
        if in_string:
            out.append(char)
            if char == "\\":
                out.append(text[index + 1])
                index += 2
                continue
            if char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            index += 1
            continue
        if text.startswith("//", index):
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        out.append(char)
        index += 1
    return "".join(out)


def sample_json(text: str) -> dict:
    block = text.split("```jsonc", 1)[1].split("```", 1)[0]
    return json.loads(strip_jsonc(block))


def split_sections(text: str) -> dict[str, str]:
    """{section: its text}, bounded by the next heading of any level."""
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        if SECTION_HEADING.match(line):
            named = SECTION_NAME.findall(SECTION_HEADING.match(line).group(1))
            current = named[0] if len(named) == 1 and named[0] in KNOWN_SECTIONS else None
            if current:
                sections.setdefault(current, [])
            continue
        if current:
            sections[current].append(line)
    return {name: "\n".join(lines) for name, lines in sections.items()}


def documented_fields(text: str) -> dict[str, str | None]:
    """{field: documented default cell}, from table rows and bullet lists alike."""
    found: dict[str, str | None] = {}
    for line in text.splitlines():
        if (row := TABLE_ROW.match(line)) is not None:
            found.setdefault(row.group(1), row.group(3).strip() or None)
            continue
        if (bullet := BULLET_FIELD.match(line)) is not None:
            body = bullet.group(2)
            default = BULLET_DEFAULT.search(body)
            found.setdefault(bullet.group(1), default.group(1) if default else None)
    return found


def literal(cell: str | None) -> object | None:
    """The cell as a value, or None when it is prose rather than a literal."""
    if cell is None:
        return None
    text = cell.replace("`", "").strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def keys_of(value: object) -> set[str]:
    return set(value.keys()) if isinstance(value, dict) else set()


def check_sample(sample: dict, seed: dict, lang: str, problems: list[str]) -> None:
    documented = keys_of(sample.get("server"))
    expected = keys_of(seed["server"])
    for missing in sorted(expected - documented):
        problems.append(f"{lang}: sample `server.{missing}` is undocumented but the binary writes it")
    for invented in sorted(documented - expected):
        problems.append(f"{lang}: sample `server.{invented}` does not exist in the config")
    for field in sorted(expected & documented):
        if sample["server"][field] != seed["server"][field]:
            problems.append(
                f"{lang}: sample `server.{field}` = {sample['server'][field]!r}, "
                f"the default is {seed['server'][field]!r}"
            )

    def cover(path: str, shown: object, emitted: object) -> None:
        if isinstance(emitted, dict):
            if not isinstance(shown, dict):
                problems.append(f"{lang}: sample `{path}` should be an object")
                return
            for missing in sorted(keys_of(emitted) - keys_of(shown)):
                problems.append(f"{lang}: sample `{path}.{missing}` is undocumented but the binary writes it")
            for field in sorted(keys_of(emitted) & keys_of(shown)):
                cover(f"{path}.{field}", shown[field], emitted[field])
        elif isinstance(emitted, list) and emitted:
            if not isinstance(shown, list) or not shown:
                problems.append(f"{lang}: sample `{path}` should be a non-empty array")
                return
            cover(f"{path}[0]", shown[0], emitted[0])

    for section in ("providers", "routes"):
        cover(section, sample.get(section), seed.get(section))


def check_coverage_and_defaults(text: str, defaults: dict, lang: str, problems: list[str],
                                prose: list[str],
                                required_paths_seen: set[str] | None = None) -> None:
    sections = split_sections(text)
    objects = {
        "server": defaults["server"],
        # A route's target fields are documented in the routes section.
        "providers": defaults["providers"][0],
        "routes": {**defaults["routes"][0], **defaults["routes"][0]["targets"][0]},
    }
    for section, fields in objects.items():
        body = sections.get(section)
        if body is None:
            problems.append(f"{lang}: no `{section}` section found")
            continue
        shown = documented_fields(body)
        for name in sorted(keys_of(fields)):
            if name not in shown:
                problems.append(f"{lang}: `{section}.{name}` is not documented (table row or bullet)")
        for name in sorted(shown):
            cell = shown[name]
            # `siblings` is what a `` `field` `` reference can point at, so it is
            # the documented set rather than the emitted one. `fields` is only
            # used where an emitted value has to be compared, and a field the
            # minimal fixture does not emit (because the writer omits it at its
            # default) still gets its prose classified — otherwise a prose cell
            # for exactly those fields would never be examined at all.
            siblings = dict.fromkeys(shown)
            if cell is None or literal(cell) is None:
                form, argument = classify_prose(cell)
                verdict = check_prose(form, argument, section, name, cell, siblings, body, lang,
                                      problems, required_paths_seen)
                prose.append((verdict, f"{lang}: `{section}.{name}` default {cell!r} [{form}]"))
                continue
            if name not in fields:
                continue
            value = literal(cell)
            actual = fields[name]
            # A literal that happens to equal the JSON default can still be the
            # wrong row: `base_url` defaults to "" *and* is rejected when empty,
            # so documenting `""` passes the equality test above while telling
            # the reader the opposite of what the validator enforces. When the
            # report is available, a field it complains about must be documented
            # as required rather than given a value.
            if required_paths_seen is not None and unconditionally_required(
                    required_paths_seen, section, name):
                problems.append(
                    f"{lang}: `{section}.{name}` documents default {cell!r}, but `config "
                    f"validate` rejects the field when it is empty — the row should say it "
                    f"is required")
                continue
            if not isinstance(actual, (dict, list)) and value != actual:
                problems.append(
                    f"{lang}: `{section}.{name}` documents default {cell!r}, "
                    f"the binary reports {actual!r}"
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", required=True, type=pathlib.Path,
                        help="config.json written by `literouter config init`")
    parser.add_argument("--defaults", required=True, type=pathlib.Path,
                        help="`literouter serve --print-config --force` for a minimal config")
    parser.add_argument("--docs", default=pathlib.Path("docs"), type=pathlib.Path)
    parser.add_argument("--status", type=pathlib.Path,
                        help="a real GET /__literouter/status body; validates the sample in "
                             "protocols-api.md against it")
    parser.add_argument("--validate-report", type=pathlib.Path,
                        help="`literouter config validate --json` for a config with a provider "
                             "that has neither id nor base_url; upgrades the `required` prose "
                             "cells from recognised to verified")
    args = parser.parse_args()

    seed = json.loads(args.seed.read_text(encoding="utf-8"))
    defaults = json.loads(args.defaults.read_text(encoding="utf-8"))
    if not defaults.get("providers") or not defaults.get("routes"):
        print("--defaults must come from a config with one provider and one route",
              file=sys.stderr)
        return 2

    problems: list[str] = []
    prose: list[tuple[str, str]] = []
    required_paths_seen = None
    if args.validate_report is not None:
        report = json.loads(args.validate_report.read_text(encoding="utf-8"))
        required_paths_seen = required_paths(report)
    for lang in ("zh", "en"):
        text = (args.docs / lang / "configuration.md").read_text(encoding="utf-8")
        check_sample(sample_json(text), seed, lang, problems)
        check_coverage_and_defaults(text, defaults, lang, problems, prose, required_paths_seen)

    if args.status is not None:
        real = json.loads(args.status.read_text(encoding="utf-8"))
        for lang in ("zh", "en"):
            text = (args.docs / lang / "protocols-api.md").read_text(encoding="utf-8")
            sample = json.loads(text.split("GET /__literouter/status", 1)[1]
                                    .split("```json", 1)[1].split("```", 1)[0])
            if set(sample) != set(real):
                problems.append(
                    f"{lang}: the status sample and a real reply disagree on their keys: "
                    f"only documented {sorted(set(sample) - set(real))}, "
                    f"only real {sorted(set(real) - set(sample))}")
            # A bucket is only in the reply once there has been traffic, so the
            # sample's shape is checked against the serializer's own keys instead.
            if sample.get("hourly"):
                bucket_keys = {"hour_unix", "bucket_sec", "requests", "successes", "failures",
                               "bytes_out", "tokens_prompt", "tokens_completion", "cost_usd"}
                if set(sample["hourly"][0]) != bucket_keys:
                    problems.append(f"{lang}: the hourly sample's keys are not {sorted(bucket_keys)}")

    if problems:
        print(f"configuration.md disagrees with the build in {len(problems)} place(s):\n")
        for problem in problems:
            print(f"  - {problem}")
        print("\nThe binary is the source of truth: `config init` and "
              "`serve --print-config` are what it emits.")
        return 1

    print("configuration.md matches the build" + (" and protocols-api.md matches a real reply"
                                                  if args.status is not None else ""))
    print(f"  checked: {len(seed['server'])} server, {len(defaults['providers'][0])} provider, "
          f"{len(defaults['routes'][0])} route and "
          f"{len(defaults['routes'][0]['targets'][0])} target fields, zh and en")
    print("  note: a field the JSON writer only emits when it is not at its default "
          "(protocol, headers) cannot be enumerated from a default config, so its "
          "table row is compared but its presence is not required by this check")
    verified = [entry for verdict, entry in prose if verdict == "checked"]
    recognised = [entry for verdict, entry in prose if verdict == "recognised"]
    print(f"  prose default cells: {len(prose)} "
          f"({len(verified)} verified against the build, {len(recognised)} recognised)")
    for entry in verified:
        print(f"    - verified: {entry}")
    for entry in recognised:
        print(f"    - recognised, not verified here (an empty cell is a claim of "
              f"'no default', which the build's output does not enumerate): {entry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
