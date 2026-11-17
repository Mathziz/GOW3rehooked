# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the existing Ghidra analysis through read-only ghidra-mcp v6 APIs.

python tools/etlprof/export-guest-symbols.py build-fast/guest-symbols/eboot.tsv
The .starts bitmap records instruction boundaries for checking a capture's load base.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--server", default="http://127.0.0.1:8089")
    parser.add_argument("--program", default="eboot.bin")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()

    def get(endpoint, **params):
        query = urllib.parse.urlencode(dict(program=args.program, **params))
        with urllib.request.urlopen(f"{args.server}/{endpoint}?{query}", timeout=120) as response:
            text = response.read().decode("utf-8")
        try:
            result = json.loads(text)
        except json.JSONDecodeError:
            result = text
        if isinstance(result, dict) and "error" in result:
            raise RuntimeError(result)
        return result

    info = get("get_current_program_info")
    base = int(info["image_base"], 16)
    options = {o["name"]: o["value"] for o in get("get_program_options", group="Program Information")["options"]}
    functions = get("list_functions_enhanced", offset=0, limit=100000)["functions"]
    if len(functions) != info["function_count"]:
        raise RuntimeError("Function list was truncated or the program changed during export")
    functions = [f for f in functions if not f["isExternal"] and "::" not in f["address"]]

    def inspect(function):
        entry = function["address"]
        body = get("get_function_by_address", address=entry)
        match = re.search(r"Body: ([0-9a-fA-F]+) - ([0-9a-fA-F]+)", body)
        if not match:
            raise RuntimeError(f"No body for {entry}: {body}")
        lo, hi = (int(a, 16) for a in match.groups())
        hashed = get("get_function_hash", address=entry)
        instructions = re.findall(r"^([0-9a-fA-F]+): (\S+)", get("disassemble_function", address=entry), re.M)
        return function, lo, hi, hashed, instructions

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        analyzed = []
        for n, result in enumerate(pool.map(inspect, functions), 1):
            analyzed.append(result)
            if n % 1000 == 0:
                print(f"Read {n}/{len(functions)} functions", flush=True)

    mnemonics = sorted({m for *_, instructions in analyzed for _, m in instructions})

    def export_body(record):
        function, lo, hi, hashed, instructions = record
        ranges, starts = [], set()
        entry = int(function["address"], 16)
        name = re.sub(r"[\t\r\n]", " ", function["name"])
        if lo == entry and hi - lo + 1 == hashed["size_bytes"]:
            if len(instructions) != hashed["instruction_count"]:
                raise RuntimeError(f"Instruction count mismatch at {entry:x}")
            starts.update(int(a, 16) - base for a, _ in instructions)
            ranges.append((lo - base, hi - base + 1, entry - base, name))
        else:
            # The body min/max API loses holes. Query exact body members instead.
            members = []
            for mnemonic in sorted({m for _, m in instructions}) if lo == entry else mnemonics:
                result = get("search_instructions", function=f"{entry:x}", mnemonic=mnemonic, limit=50000)
                if result["truncated"]:
                    raise RuntimeError(f"Truncated body at {entry:x}")
                members.extend((int(i["address"]["address"], 16), i["length"]) for i in result["matches"])
            if len(members) != hashed["instruction_count"]:
                raise RuntimeError(f"Incomplete body at {entry:x}")
            for address, length in sorted(members):
                starts.add(address - base)
                if ranges and ranges[-1][1] == address - base and ranges[-1][2] == entry - base:
                    previous = ranges.pop()
                    ranges.append((previous[0], address - base + length, entry - base, name))
                else:
                    ranges.append((address - base, address - base + length, entry - base, name))

        return ranges, starts

    ranges, starts = [], set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for n, (body, boundaries) in enumerate(pool.map(export_body, analyzed), 1):
            ranges.extend(body)
            starts.update(boundaries)
            if n % 1000 == 0:
                print(f"Exported {n}/{len(functions)} bodies", flush=True)
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise RuntimeError(f"Overlapping function ranges: {previous}, {current}")
    bitmap = bytearray((max(starts) + 8) // 8)
    for address in starts:
        bitmap[address // 8] |= 1 << (address % 8)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# guest-symbols-v1\n# module\t{info['name']}\n# image_base\t{base:x}\n")
        output.write(f"# sha256\t{options['Executable SHA256']}\n")
        # Loaded blocks (prdprof names data addresses with them); overlays and Ghidra's EXTERNAL block are not mapped.
        for name, lo, hi in re.findall(r"^(\.\w+): ([0-9a-fA-F]+) - ([0-9a-fA-F]+)$", get("list_segments", limit=100), re.M):
            output.write(f"# segment\t{name}\t{int(lo, 16) - base:x}\t{int(hi, 16) + 1 - base:x}\n")
        output.write("# start_rva\tend_rva_exclusive\tentry_rva\tname\n")
        for lo, hi, entry, name in ranges:
            output.write(f"{lo:x}\t{hi:x}\t{entry:x}\t{name}\n")
    Path(str(args.output) + ".starts").write_bytes(bitmap)
    print(f"Exported {len(functions)} functions, {len(ranges)} exact ranges, {len(starts)} instruction starts. "
          f"SHA256 {options['Executable SHA256']}", flush=True)


if __name__ == "__main__":
    main()
