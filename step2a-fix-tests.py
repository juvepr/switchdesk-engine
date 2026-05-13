#!/usr/bin/env python3
"""
SwitchDesk engine — Phase 1 step 2a addendum
Fix tests/unit/test_video.cpp after the 2a encoder cuts.

The test file's EncoderTest fixture parameterizes over &video::nvenc,
&video::amdvce, &video::quicksync, &video::vaapi, &video::videotoolbox,
and &video::software. After step 2a, only &video::nvenc remains; the others
are undefined references and break the test_sunshine.exe link step (the
main sunshine.exe target is unaffected and builds cleanly).

This script:
  - Collapses the EncoderTest parameter list to just &video::nvenc,
    dropping the platform #ifdefs that wrapped each encoder.
  - Simplifies SetUp(): the "if encoder.name == software FAIL else SKIP"
    branch is meaningless with only NVENC; collapse to a single GTEST_SKIP
    if NVENC isn't available on the host. (Test machines without NVIDIA
    hardware should skip, not fail.)

The actual ValidateEncoder test body remains a `todo` placeholder — step 8
(simplify probing) is the natural place to put real assertions there.

The FramerateX100Test below the encoder fixture is untouched.

Idempotent. Run from the engine repo root.
"""
import sys
from pathlib import Path

PATH = Path("tests/unit/test_video.cpp")


def edit(content, *, find, replace, description):
    count = content.count(find)
    if count == 0:
        print(f"  SKIP  {description}")
        return content
    if count > 1:
        raise RuntimeError(
            f"anchor matched {count} times for {description!r}; aborting"
        )
    print(f"  EDIT  {description}")
    return content.replace(find, replace, 1)


def main():
    if not PATH.exists():
        sys.exit(f"ERROR: {PATH} not found. Run from the engine repo root.")

    raw = PATH.read_bytes()
    content = raw.decode("utf-8")
    if "\r\n" in content:
        content = content.replace("\r\n", "\n")
        print("  (normalized CRLF → LF for processing)")
    original = content
    starting_lines = content.count("\n") + 1
    print(f"  {PATH}: {starting_lines} lines, {len(raw)} bytes")

    # ------------------------------------------------------------------
    # Edit 1: Collapse the EncoderTest parameter list to NVENC-only.
    # ------------------------------------------------------------------
    content = edit(
        content,
        find=(
            "  testing::Values(\n"
            "#if !defined(__APPLE__)\n"
            "    &video::nvenc,\n"
            "#endif\n"
            "#ifdef _WIN32\n"
            "    &video::amdvce,\n"
            "    &video::quicksync,\n"
            "#endif\n"
            "#if defined(__linux__) || defined(__FreeBSD__)\n"
            "    &video::vaapi,\n"
            "#endif\n"
            "#ifdef __APPLE__\n"
            "    &video::videotoolbox,\n"
            "#endif\n"
            "    &video::software\n"
            "  ),\n"
        ),
        replace=(
            "  testing::Values(\n"
            "    &video::nvenc\n"
            "  ),\n"
        ),
        description="fix.1  collapse EncoderTest parameters to NVENC-only",
    )

    # ------------------------------------------------------------------
    # Edit 2: Simplify SetUp() — drop the software-fallback branch.
    # ------------------------------------------------------------------
    content = edit(
        content,
        find=(
            "  void SetUp() override {\n"
            "    auto &encoder = *GetParam();\n"
            "    if (!video::validate_encoder(encoder, false)) {\n"
            "      // Encoder failed validation,\n"
            "      // if it's software - fail, otherwise skip\n"
            "      if (encoder.name == \"software\") {\n"
            "        FAIL() << \"Software encoder not available\";\n"
            "      } else {\n"
            "        GTEST_SKIP() << \"Encoder not available\";\n"
            "      }\n"
            "    }\n"
            "  }\n"
        ),
        replace=(
            "  void SetUp() override {\n"
            "    auto &encoder = *GetParam();\n"
            "    if (!video::validate_encoder(encoder, false)) {\n"
            "      GTEST_SKIP() << \"Encoder not available\";\n"
            "    }\n"
            "  }\n"
        ),
        description="fix.2  simplify SetUp() — drop software-fallback branch",
    )

    # ------------------------------------------------------------------
    # Write back.
    # ------------------------------------------------------------------
    if content == original:
        print("\n  No changes (already applied?).")
        return

    final_lines = content.count("\n") + 1
    bytes_saved = len(raw) - len(content.encode("utf-8"))
    print(
        f"\n  Result: {final_lines} lines "
        f"(was {starting_lines}, Δ {starting_lines - final_lines}), "
        f"{bytes_saved} bytes saved"
    )

    PATH.write_bytes(content.encode("utf-8"))
    print(f"  Wrote {PATH} (LF endings).")


if __name__ == "__main__":
    main()
