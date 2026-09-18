#!/usr/bin/env python3
"""Check docs/{zh,en}/configuration.md against the config a build actually produces.

The sample JSON, the field tables and the code are three copies of the same
facts, and the copy that is not compiled is the one that drifts: `log_capacity`
was documented as 400 while the code defaulted to 200 (`ServerConfig` and
`test_config` both said so), `language` and `ui_scale` were read, written,
validated and in neither table nor sample, and a macOS config path was invented
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
KNOWN_SECTIONS = ("server", "providers", "routes", "clients", "client_keys")


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
                                prose: list[str]) -> None:
    sections = split_sections(text)
    objects = {
        "server": defaults["server"],
        # A route's target fields are documented in the routes section.
        "providers": defaults["providers"][0],
        "routes": {**defaults["routes"][0], **defaults["routes"][0]["targets"][0]},
    }
    if defaults.get("clients"):
        objects["clients"] = defaults["clients"][0]
        if defaults["clients"][0].get("keys"):
            objects["client_keys"] = defaults["clients"][0]["keys"][0]
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
            if name not in fields:
                continue
            cell = shown[name]
            if cell is None or literal(cell) is None:
                prose.append(f"{lang}: `{section}.{name}` default {cell!r}")
                continue
            value = literal(cell)
            actual = fields[name]
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
    args = parser.parse_args()

    seed = json.loads(args.seed.read_text(encoding="utf-8"))
    defaults = json.loads(args.defaults.read_text(encoding="utf-8"))
    if not defaults.get("providers") or not defaults.get("routes"):
        print("--defaults must come from a config with one provider and one route",
              file=sys.stderr)
        return 2

    problems: list[str] = []
    prose: list[str] = []
    for lang in ("zh", "en"):
        text = (args.docs / lang / "configuration.md").read_text(encoding="utf-8")
        check_sample(sample_json(text), seed, lang, problems)
        check_coverage_and_defaults(text, defaults, lang, problems, prose)

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
                bucket_keys = {"hour_unix", "requests", "successes", "failures", "bytes_out",
                               "tokens_prompt", "tokens_completion", "cost_usd"}
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
    if defaults.get("clients"):
        client = defaults["clients"][0]
        print(f"  distribution: {len(client)} client and "
              f"{len(client.get('keys', [{}])[0]) if client.get('keys') else 0} client-key fields, zh and en")
    print("  note: a field the JSON writer only emits when it is not at its default "
          "(protocol, headers) cannot be enumerated from a default config, so its "
          "table row is compared but its presence is not required by this check")
    print(f"  not verifiable (documented in prose): {len(prose)} cell(s)")
    for entry in prose:
        print(f"    - {entry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
