#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = "3574d6e9ca26f05fd7f88cb675eaabeb28804a9bc6b7f10b592df5a4f13580a9"


def main():
    parser = argparse.ArgumentParser(description="Check actual 8796 make flag expansion, which must not silently drop native bootstrap definitions")
    parser.add_argument("--make-def", required=True, type=Path)
    args = parser.parse_args()
    original = args.make_def.read_bytes()
    if hashlib.sha256(original).hexdigest() != SOURCE_SHA256:
        raise ValueError("unqualified 8796 MakeInc.def")
    delta = (ROOT / "ci/unixnu-16x.patch").read_text().split(
        "diff --git a/patches/xnu-8796/native-build-flags.patch b/patches/xnu-8796/native-build-flags.patch\n", 1)[1]
    delta = delta.split("\ndiff --git ", 1)[0].splitlines()[4:]
    patch = "\n".join(line[1:] for line in delta) + "\n"
    with tempfile.TemporaryDirectory(prefix="lumia-xnu-flags-") as temporary:
        directory = Path(temporary)
        source = directory / "makedefs/MakeInc.def"
        source.parent.mkdir()
        source.write_bytes(original)
        subprocess.run(["git", "apply", "--whitespace=nowarn", "-"], input=patch, text=True, cwd=directory, check=True)
        for label, text, expected in [("original", original.decode(), False), ("patched", source.read_text(), True)]:
            flags = []
            for variable in ["CFLAGS", "SFLAGS"]:
                definition = re.search(r"^" + variable + r"\s*=(?:[^\n]*\\\n)*[^\n]*", text, re.M)
                if not definition:
                    raise ValueError(f"missing actual {variable} definition")
                flags.append(definition.group())
                flags.extend(re.findall(r"^" + variable + r" \+= .*EXTRA.*$", text, re.M))
            makefile = directory / f"{label}.mk"
            makefile.write_text("CFLAGS_GEN=-DKEPT_C=7\nSFLAGS_GEN=-DKEPT_ASM=9\n" + "\n".join(flags) +
                "\nall:\n\t@printf '%s\\n' '$(CFLAGS)' '$(SFLAGS)'\n")
            result = subprocess.check_output(["make", "--no-print-directory", "-f", str(makefile),
                "CFLAGS_EXTRA=-DXNU_SOURCE_CORECRYPTO14=1", "SFLAGS_EXTRA=-DNATIVE_ASM_TEST=1"], text=True)
            c_flags, assembly_flags = result.splitlines()
            assert "-DKEPT_C=7" in c_flags and "-DKEPT_ASM=9" in assembly_flags
            assert ("-DXNU_SOURCE_CORECRYPTO14=1" in c_flags) == expected
            assert ("-DNATIVE_ASM_TEST=1" in assembly_flags) == expected
    if args.make_def.read_bytes() != original:
        raise ValueError("upstream source mutated")
    print("PASS: real 8796 make flags preserve existing definitions and carry the requested native source initializer")


if __name__ == "__main__":
    main()
