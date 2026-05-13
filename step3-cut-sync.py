#!/usr/bin/env python3
"""
Phase 1 step 3: cut sync-mode code from src/video.cpp.

Idempotent: re-running on an already-cut file prints SKIPs and exits 0.
Strict: any anchor that should-be-unique but isn't unique => fail-fast.
Normalizes LF for editing; restores CRLF on write if the original had CRLF
(repo index is LF, but working tree on Windows checks out CRLF — lesson
from decisions.md 2026-05-12 line-endings entry).

Run from the engine repo root:
    python3 step3-cut-sync.py
"""

import sys
import pathlib

PATH = pathlib.Path("src/video.cpp")


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def delete_block(src, label, start_anchor, end_anchor):
    """Delete from start_anchor (inclusive) through end_anchor (inclusive),
    plus one trailing newline. Both anchors must each appear exactly once."""
    sc, ec = src.count(start_anchor), src.count(end_anchor)
    if sc == 0:
        print(f"  SKIP  {label}: start anchor not present (already cut?)")
        return src
    if sc > 1:
        fail(f"{label}: start anchor appears {sc} times")
    if ec == 0:
        fail(f"{label}: end anchor not present")
    if ec > 1:
        fail(f"{label}: end anchor appears {ec} times")

    start = src.index(start_anchor)
    end = src.index(end_anchor, start)
    if end < start:
        fail(f"{label}: end anchor appears before start anchor")
    end += len(end_anchor)
    if end < len(src) and src[end] == "\n":
        end += 1

    n_lines = src[start:end].count("\n")
    print(f"  CUT   {label}: {n_lines} lines")
    return src[:start] + src[end:]


def delete_line(src, label, line):
    """Delete a single full line (newline included). Must appear exactly once."""
    full = line if line.endswith("\n") else line + "\n"
    c = src.count(full)
    if c == 0:
        print(f"  SKIP  {label}: line not present")
        return src
    if c > 1:
        fail(f"{label}: line appears {c} times")
    print(f"  CUT   {label}: 1 line")
    return src.replace(full, "", 1)


def replace_unique(src, label, old, new):
    c = src.count(old)
    if c == 0:
        if new in src:
            print(f"  SKIP  {label}: already replaced")
            return src
        fail(f"{label}: old text not found")
    if c > 1:
        fail(f"{label}: old text appears {c} times")
    delta = old.count("\n") - new.count("\n")
    print(f"  REPL  {label}: -{delta} lines")
    return src.replace(old, new, 1)


def main():
    if not PATH.exists():
        fail(f"{PATH} not found — run from engine repo root")

    raw = PATH.read_bytes()
    had_crlf = b"\r\n" in raw
    src = raw.decode("utf-8").replace("\r\n", "\n")
    before_lines = src.count("\n")
    print(f"Loaded {PATH}: {before_lines} lines, CRLF={had_crlf}")

    # ===== Type declarations and globals (top of file) =====

    # 1. struct sync_session_ctx_t
    src = delete_block(
        src, "struct sync_session_ctx_t",
        "  struct sync_session_ctx_t {\n",
        "    void *channel_data;\n  };",
    )

    # 2. struct sync_session_t
    src = delete_block(
        src, "struct sync_session_t",
        "  struct sync_session_t {\n",
        "    std::unique_ptr<encode_session_t> session;\n  };",
    )

    # 3. using encode_session_ctx_queue_t (only used by sync)
    src = delete_line(
        src, "using encode_session_ctx_queue_t",
        "  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;",
    )

    # 4. using encode_e (only used by encode_run_sync / captureThreadSync)
    src = delete_line(
        src, "using encode_e",
        "  using encode_e = platf::capture_e;",
    )

    # 5. struct capture_thread_sync_ctx_t
    src = delete_block(
        src, "struct capture_thread_sync_ctx_t",
        "  struct capture_thread_sync_ctx_t {\n",
        "    encode_session_ctx_queue_t encode_session_ctx_queue {30};\n  };",
    )

    # 6a. forward decl: start_capture_sync
    src = delete_line(
        src, "forward decl start_capture_sync",
        "  int start_capture_sync(capture_thread_sync_ctx_t &ctx);",
    )

    # 6b. forward decl: end_capture_sync
    src = delete_line(
        src, "forward decl end_capture_sync",
        "  void end_capture_sync(capture_thread_sync_ctx_t &ctx);",
    )

    # 7. global capture_thread_sync
    src = delete_line(
        src, "global capture_thread_sync",
        "  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);",
    )

    # ===== Function bodies =====

    # 8. make_synced_session()
    src = delete_block(
        src, "make_synced_session()",
        "  std::optional<sync_session_t> make_synced_session(platf::display_t *disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {\n",
        "    return encode_session;\n  }",
    )

    # 9. encode_run_sync()
    src = delete_block(
        src, "encode_run_sync()",
        "  encode_e encode_run_sync(\n",
        "    return encode_e::ok;\n  }",
    )

    # 10. captureThreadSync()
    src = delete_block(
        src, "captureThreadSync()",
        "  void captureThreadSync() {\n",
        "    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {}\n  }",
    )

    # 11. capture() dispatcher: collapse if/else to unconditional async call
    src = replace_unique(
        src, "capture() if/else collapse",
        "    idr_events->raise(true);\n"
        "    if (chosen_encoder->flags & PARALLEL_ENCODING) {\n"
        "      capture_async(std::move(mail), config, channel_data);\n"
        "    } else {\n"
        "      safe::signal_t join_event;\n"
        "      auto ref = capture_thread_sync.ref();\n"
        "      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {\n"
        "        &join_event,\n"
        "        mail->event<bool>(mail::shutdown),\n"
        "        mail::man->queue<packet_t>(mail::video_packets),\n"
        "        std::move(idr_events),\n"
        "        mail->event<hdr_info_t>(mail::hdr),\n"
        "        mail->event<input::touch_port_t>(mail::touch_port),\n"
        "        config,\n"
        "        1,\n"
        "        channel_data,\n"
        "      });\n"
        "\n"
        "      // Wait for join signal\n"
        "      join_event.view();\n"
        "    }\n",
        "    idr_events->raise(true);\n"
        "    capture_async(std::move(mail), config, channel_data);\n",
    )

    # 12. start_capture_sync() definition — short body, match in full
    src = replace_unique(
        src, "start_capture_sync() definition",
        "  int start_capture_sync(capture_thread_sync_ctx_t &ctx) {\n"
        "    std::thread {&captureThreadSync}.detach();\n"
        "    return 0;\n"
        "  }\n"
        "\n",
        "",
    )

    # 13. end_capture_sync() definition — empty body, match in full
    src = replace_unique(
        src, "end_capture_sync() definition",
        "  void end_capture_sync(capture_thread_sync_ctx_t &ctx) {\n"
        "  }\n"
        "\n",
        "",
    )

    # ===== Post-pass: collapse runs of blank lines =====
    # Original file has zero multi-blank-line runs; our deletions can stack
    # trailing newlines. Collapse any run of 2+ blanks to a single blank.
    import re
    src, n_collapsed = re.subn(r"\n{3,}", "\n\n", src)
    if n_collapsed:
        print(f"  POST  collapsed {n_collapsed} multi-blank-line runs")

    # ===== Write back =====
    after_lines = src.count("\n")
    print(f"Done: {before_lines} -> {after_lines} lines (-{before_lines - after_lines})")

    if had_crlf:
        out = src.replace("\n", "\r\n").encode("utf-8")
    else:
        out = src.encode("utf-8")
    PATH.write_bytes(out)


if __name__ == "__main__":
    main()
