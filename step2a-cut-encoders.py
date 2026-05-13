#!/usr/bin/env python3
"""
SwitchDesk engine — Phase 1 step 2a
Cut non-NVENC encoder declarations and collapse the encoders vector.

Targets:  src/video.cpp at master b83e9ed3 (post-1c, 2872 lines).
Run from: engine repo root (where src/video.cpp exists).

After this script runs cleanly:
  - encoder_t nvenc remains, unwrapped from its #ifdef _WIN32 / #endif guard
  - encoder_t quicksync, amdvce, mediafoundation, software DECLARATIONS gone
  - encoders vector collapsed to { &nvenc }

This script does NOT touch:
  - avcodec_software_encode_device_t / avcodec_encode_session_t classes
  - encode_avcodec / make_avcodec_encode_session functions
  - encode() / make_encode_session() dispatchers
  - bottom-of-file avcodec hardware buffer init helpers
  - namespace nv / namespace qsv
  - map_base_dev_type cases

Those all go in step 2b. After 2a, the build still has the avcodec session
infrastructure, but it's unreachable at runtime (no encoder references it).

Idempotent: every edit checks for its anchor and skips if not present. Safe
to re-run. Fails loudly if an anchor matches multiple times or unexpectedly
disappears mid-script.

Line endings: reads as bytes, normalizes CRLF→LF before processing, writes
back as LF. Matches the engine repo's git index encoding. (1c lesson.)
"""
import sys
from pathlib import Path

PATH = Path("src/video.cpp")


def edit(content, *, find, replace, description):
    """Literal find/replace. Idempotent: skip if `find` is absent.
    Fails if `find` matches more than once."""
    count = content.count(find)
    if count == 0:
        print(f"  SKIP  {description}")
        return content
    if count > 1:
        raise RuntimeError(
            f"anchor matched {count} times for {description!r} "
            f"(expected exactly 1); aborting"
        )
    print(f"  EDIT  {description}")
    return content.replace(find, replace, 1)


def cut_block(content, *, start, end, replacement, description):
    """Cut the inclusive range [start..end] and substitute `replacement`.
    Idempotent: skip if `start` is absent.
    Fails if `start` matches more than once, or if `end` not found after `start`."""
    s = content.find(start)
    if s == -1:
        print(f"  SKIP  {description}")
        return content
    s2 = content.find(start, s + 1)
    if s2 != -1:
        raise RuntimeError(
            f"start anchor matched multiple times for {description!r}; aborting"
        )
    e = content.find(end, s + len(start))
    if e == -1:
        raise RuntimeError(
            f"end anchor not found after start anchor for {description!r}; aborting"
        )
    end_pos = e + len(end)
    print(f"  CUT   {description}: removed {end_pos - s} chars")
    return content[:s] + replacement + content[end_pos:]


def main():
    if not PATH.exists():
        sys.exit(
            f"ERROR: {PATH} not found. Run this script from the engine repo root."
        )

    raw = PATH.read_bytes()
    content = raw.decode("utf-8")
    had_crlf = "\r\n" in content
    if had_crlf:
        content = content.replace("\r\n", "\n")
        print("  (normalized CRLF → LF for processing)")
    original = content
    starting_lines = content.count("\n") + 1
    print(f"  src/video.cpp: {starting_lines} lines, {len(raw)} bytes")

    # ------------------------------------------------------------------
    # Edit 1: Remove the #ifdef _WIN32 immediately above encoder_t nvenc.
    # Net effect: nvenc no longer wrapped in its outer #ifdef.
    # ------------------------------------------------------------------
    content = edit(
        content,
        find="#ifdef _WIN32\n  encoder_t nvenc {",
        replace="  encoder_t nvenc {",
        description="2a.1  drop opening #ifdef _WIN32 above encoder_t nvenc",
    )

    # ------------------------------------------------------------------
    # Edit 2: Cut the entire quicksync/amdvce/mediafoundation block,
    # including nvenc's matching #endif AND the #ifdef/#endif wrapper
    # surrounding the three encoder declarations.
    #
    # The anchor span is:
    #   <start>  };           (nvenc's closing brace)
    #            #endif       (nvenc wrapper's closing #endif)
    #            (blank)
    #            #ifdef _WIN32
    #              encoder_t quicksync { ... };
    #              encoder_t amdvce { ... };
    #              encoder_t mediafoundation { ... };
    #            #endif       (qsv wrapper's closing #endif)
    #            (blank)
    #            encoder_t software {  <end>
    #
    # The end anchor reaches into `encoder_t software {` so the cut
    # neatly transitions to the software declaration, which we delete
    # in Edit 3.
    # ------------------------------------------------------------------
    content = cut_block(
        content,
        start="  };\n#endif\n\n#ifdef _WIN32\n  encoder_t quicksync {",
        end="    PARALLEL_ENCODING | FIXED_GOP_SIZE  // MF encoder doesn't support on-demand IDR frames\n  };\n#endif\n\n  encoder_t software {",
        replacement="  };\n\n  encoder_t software {",
        description="2a.2  cut nvenc's trailing #endif + quicksync/amdvce/mediafoundation",
    )

    # ------------------------------------------------------------------
    # Edit 3: Delete the encoder_t software declaration entirely.
    # The trailing blank line is absorbed into the end-anchor so the
    # transition into the encoders vector remains a single blank line.
    # ------------------------------------------------------------------
    content = cut_block(
        content,
        start="  encoder_t software {",
        end="    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT\n  };\n\n",
        replacement="",
        description="2a.3  cut encoder_t software",
    )

    # ------------------------------------------------------------------
    # Edit 4: Collapse the encoders vector to NVENC-only.
    # Removes the `#ifdef _WIN32` block listing the three avcodec encoders
    # and the trailing `&software` entry. Trailing comma after `&nvenc`
    # is also dropped (would be a syntax error otherwise).
    # ------------------------------------------------------------------
    content = edit(
        content,
        find=(
            "  static const std::vector<encoder_t *> encoders {\n"
            "    &nvenc,\n"
            "#ifdef _WIN32\n"
            "    &quicksync,\n"
            "    &amdvce,\n"
            "    &mediafoundation,\n"
            "#endif\n"
            "    &software\n"
            "  };"
        ),
        replace=(
            "  static const std::vector<encoder_t *> encoders {\n"
            "    &nvenc\n"
            "  };"
        ),
        description="2a.4  collapse encoders vector to { &nvenc }",
    )

    # ------------------------------------------------------------------
    # Write back.
    # ------------------------------------------------------------------
    if content == original:
        print("\n  No changes (all anchors already absent — already applied?).")
        return

    final_lines = content.count("\n") + 1
    bytes_saved = len(raw) - len(content.encode("utf-8"))
    print(
        f"\n  Result: {final_lines} lines "
        f"(was {starting_lines}, Δ {starting_lines - final_lines}), "
        f"{bytes_saved} bytes saved"
    )

    # Always write LF, regardless of working-tree encoding. Matches the index.
    PATH.write_bytes(content.encode("utf-8"))
    print(f"  Wrote {PATH} (LF endings).")


if __name__ == "__main__":
    main()
