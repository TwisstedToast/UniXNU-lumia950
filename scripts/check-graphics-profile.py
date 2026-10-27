import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
NUMBER = r"(?:Zero|One|0x[0-9a-fA-F]+|[0-9]+)"


def integer(value):
    if value == "Zero":
        return 0
    if value == "One":
        return 1
    return int(value, 0)


def graphics_set(text, index):
    candidates = []
    pattern = rf'Package\s*\({NUMBER}\)\s*\{{\s*"PSTATE_SET"\s*,\s*({NUMBER})\s*,'
    for match in re.finditer(pattern, text):
        if integer(match[1]) != index:
            continue
        opening = text.index("{", match.start())
        depth = 0
        for token in re.finditer(r'"(?:\\.|[^"\\])*"|[{}]', text[opening:]):
            if token[0] == "{":
                depth += 1
            elif token[0] == "}":
                depth -= 1
            if depth == 0:
                block = text[opening:opening + token.end()]
                if '"/clk/oxili"' in block:
                    candidates.append(block)
                break
        else:
            raise ValueError("unterminated PSTATE_SET package")
    if len(candidates) != 1:
        raise ValueError(f"expected one graphics PSTATE_SET {index}, got {len(candidates)}")
    return candidates[0]


def extract_profile(text):
    active = graphics_set(text, 2)
    indices = [integer(value) for value in re.findall(rf'"PSTATE"\s*,\s*({NUMBER})\s*,', active)]
    rates = [integer(value) for value in re.findall(rf'"/clk/oxili"\s*,\s*({NUMBER})', active)]
    if indices != list(range(7)) or rates != [600000, 490000, 450000, 366000, 300000, 180000, 0]:
        raise ValueError("graphics active P-state table changed")
    policies = {}
    for name in ("PRELOAD_PSTATE", "PREPARE_PSTATE", "ABANDON_PSTATE"):
        values = re.findall(rf'"{name}"\s*,\s*({NUMBER})', active)
        if len(values) != 1 or integer(values[0]) != 4:
            raise ValueError(f"graphics {name} no longer selects state 4")
        policies[name] = 4
    wake = graphics_set(text, 1)
    wake_rates = [integer(value) for value in re.findall(rf'"/clk/oxili"\s*,\s*({NUMBER})', wake)]
    if wake_rates != [19200, 0]:
        raise ValueError("graphics wake/off clock profile changed")
    return {"active_rates_khz": rates, "policies": policies, "wake_khz": wake_rates[0], "startup_khz": rates[4]}


def main():
    parser = argparse.ArgumentParser(description="Verify the native graphics startup profile against hash-pinned Lumia firmware tables")
    parser.add_argument("--firmware-root", required=True, type=Path)
    args = parser.parse_args()
    evidence = json.loads((ROOT / "docs/hardware/2026-10-27-graphics-startup-profile.json").read_text())
    profiles = []
    for source in evidence["sources"]:
        path = args.firmware_root / source["path"]
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != source["sha256"]:
            raise ValueError(f"firmware source hash changed: {source['path']}")
        profiles.append(extract_profile(data.decode()))
    if profiles[0] != profiles[1] or profiles[0]["startup_khz"] != evidence["startup_khz"]:
        raise ValueError("original Microsoft and custom firmware startup profiles disagree")
    constants = (ROOT / "drivers/NativeAdreno418/BoardConstants.h").read_text()
    expected = {"kBootGpuHz": 1000 * evidence["startup_khz"], "kFixedGpuMicrovolts": 980000,
                "kMaximumGpuMicrovolts": 1355000}
    for name, value in expected.items():
        if not re.search(rf'constexpr adreno::u32 {name}\s*=\s*{value};', constants):
            raise ValueError(f"native profile differs from the qualified constant: {name}")
    print(json.dumps({"source_hashes_verified": True, "profiles": profiles,
                      "handset_voltage_or_rendering_qualified": False}, indent=2))


if __name__ == "__main__":
    main()
