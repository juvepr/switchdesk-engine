#!/usr/bin/env python3
"""
SwitchDesk engine — Phase 1 step 2b
Cut all avcodec encoder infrastructure plus opportunistic cleanup.

Targets:  src/video.cpp at master b83e9ed3 (post-1c), AFTER step 2a applied.
Run from: engine repo root.

Designed to run after step2a-cut-encoders.py. Anchors do not depend on
state introduced by 2a (they target regions 2a doesn't touch), so this
script will also run cleanly against pristine post-1c master — but the
intended flow is 2a then 2b on the same `cut-non-nvenc` branch as two
separate commits.

After this script runs cleanly:
  - namespace nv and namespace qsv removed (the latter dead after 2a;
    the former was already unreferenced pre-cut)
  - avcodec hardware buffer init forward declarations removed
  - avcodec_software_encode_device_t class removed
  - avcodec_encode_session_t class removed
  - encode_avcodec function removed
  - make_avcodec_encode_session function removed
  - encode() dispatcher simplified to NVENC-only
  - make_encode_session() dispatcher simplified to NVENC-only
  - vaapi_init / vulkan_init / cuda_init / vt_init function bodies removed
  - dxgi_init function + its #ifdef _WIN32 namespace-juggling wrapper
    + the orphan global do_nothing() function all removed
  - map_base_dev_type cleaned up: VAAPI, VULKAN (and its #ifdef wrapper),
    CUDA, VIDEOTOOLBOX cases removed; only D3D11VA + NONE + default remain

Defer to step 8: flag_e enum cleanup. Eight flags become dead after 2b
but cleaning them up alongside probing simplification is cleaner than
threading the change through here.

Idempotent: each edit checks for its anchor and skips if absent. Fails
loudly on multi-match or anchor disappearance. Safe to re-run.
"""
import sys
from pathlib import Path

PATH = Path("src/video.cpp")


def edit(content, *, find, replace, description):
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
        sys.exit(f"ERROR: {PATH} not found. Run from the engine repo root.")

    raw = PATH.read_bytes()
    content = raw.decode("utf-8")
    if "\r\n" in content:
        content = content.replace("\r\n", "\n")
        print("  (normalized CRLF → LF for processing)")
    original = content
    starting_lines = content.count("\n") + 1
    print(f"  src/video.cpp: {starting_lines} lines, {len(raw)} bytes")

    # ------------------------------------------------------------------
    # Cuts proceed top-to-bottom through the file.
    # ------------------------------------------------------------------

    # 2b.1  namespace nv — already unused pre-cut (zero in-file references).
    content = cut_block(
        content,
        start="  namespace nv {\n",
        end="  }  // namespace nv\n\n",
        replacement="",
        description="2b.1   remove namespace nv (already dead)",
    )

    # 2b.2  namespace qsv — dead after 2a (was only referenced inside the
    # quicksync encoder declaration).
    content = cut_block(
        content,
        start="  namespace qsv {\n",
        end="  }  // namespace qsv\n\n",
        replacement="",
        description="2b.2   remove namespace qsv (dead after 2a)",
    )

    # 2b.3  Forward declarations for avcodec hardware buffer init helpers.
    content = edit(
        content,
        find=(
            "  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);\n"
            "  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);\n"
            "  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);\n"
            "  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);\n"
            "  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);\n\n"
        ),
        replace="",
        description="2b.3   remove avcodec hardware buffer init forward decls",
    )

    # 2b.4  avcodec_software_encode_device_t class. Ends with its int offsetH
    # field (uniquely identifying); the trailing blank line is absorbed.
    content = cut_block(
        content,
        start="  class avcodec_software_encode_device_t: public platf::avcodec_encode_device_t {\n",
        end="    int offsetH;\n  };\n\n",
        replacement="",
        description="2b.4   remove class avcodec_software_encode_device_t",
    )

    # 2b.5  avcodec_encode_session_t class. Ends with the unique `int inject`
    # field (sps/vps injection state, only this class has it).
    content = cut_block(
        content,
        start="  class avcodec_encode_session_t: public encode_session_t {\n",
        end="    int inject;\n  };\n\n",
        replacement="",
        description="2b.5   remove class avcodec_encode_session_t",
    )

    # 2b.6  encode_avcodec function. Function-end anchor uses the function's
    # own closing pattern (`return 0;\n  }\n\n`) which is the first match
    # after the start anchor.
    content = cut_block(
        content,
        start="  int encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session,",
        end="    return 0;\n  }\n\n",
        replacement="",
        description="2b.6   remove function encode_avcodec",
    )

    # 2b.7  Simplify the encode() dispatcher: drop the avcodec_encode_session_t
    # branch (the type no longer exists).
    content = edit(
        content,
        find=(
            "  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {\n"
            "    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {\n"
            "      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);\n"
            "    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {\n"
            "      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);\n"
            "    }\n"
            "\n"
            "    return -1;\n"
            "  }\n"
        ),
        replace=(
            "  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {\n"
            "    if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {\n"
            "      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);\n"
            "    }\n"
            "\n"
            "    return -1;\n"
            "  }\n"
        ),
        description="2b.7   simplify encode() dispatcher to NVENC-only",
    )

    # 2b.8  make_avcodec_encode_session function. The largest single cut
    # in step 2b (~380 lines). Uses the unique signature as start anchor
    # and the function's only `return session;` as end anchor.
    content = cut_block(
        content,
        start="  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(\n    platf::display_t *disp,",
        end="    return session;\n  }\n\n",
        replacement="",
        description="2b.8   remove function make_avcodec_encode_session",
    )

    # 2b.9  Simplify the make_encode_session() dispatcher: drop the
    # avcodec_encode_device_t branch (its target function no longer exists).
    content = edit(
        content,
        find=(
            "  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device) {\n"
            "    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {\n"
            "      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));\n"
            "      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));\n"
            "    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {\n"
            "      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));\n"
            "      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));\n"
            "    }\n"
            "\n"
            "    return nullptr;\n"
            "  }\n"
        ),
        replace=(
            "  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device) {\n"
            "    if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {\n"
            "      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));\n"
            "      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));\n"
            "    }\n"
            "\n"
            "    return nullptr;\n"
            "  }\n"
        ),
        description="2b.9   simplify make_encode_session() dispatcher to NVENC-only",
    )

    # 2b.10  vaapi_init typedef + function. The "// Linux only declaration"
    # comment is preserved-then-deleted as part of the start-anchor span.
    content = cut_block(
        content,
        start="  // Linux only declaration\n  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)",
        end="    return hw_device_buf;\n  }\n\n",
        replacement="",
        description="2b.10  remove vaapi_init typedef + function body",
    )

    # 2b.11  vulkan_init's #ifdef SUNSHINE_BUILD_VULKAN block (typedef + function).
    content = cut_block(
        content,
        start="#ifdef SUNSHINE_BUILD_VULKAN\n  using vulkan_init_avcodec_hardware_input_buffer_fn",
        end="    return -1;\n  }\n#endif\n\n",
        replacement="",
        description="2b.11  remove vulkan_init #ifdef SUNSHINE_BUILD_VULKAN block",
    )

    # 2b.12  cuda_init function body.
    content = cut_block(
        content,
        start="  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {\n",
        end="    return hw_device_buf;\n  }\n\n",
        replacement="",
        description="2b.12  remove cuda_init function body",
    )

    # 2b.13  vt_init function body.
    content = cut_block(
        content,
        start="  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {\n",
        end="    return hw_device_buf;\n  }\n\n",
        replacement="",
        description="2b.13  remove vt_init function body",
    )

    # 2b.14  dxgi_init function + its namespace-juggling wrapper. The whole
    # `#ifdef _WIN32 } void do_nothing(void *) {} namespace video { ... #endif`
    # construct exists only so do_nothing() can have C linkage as a lock
    # callback for the d3d11va hwcontext. Once dxgi_init goes, it all goes.
    content = cut_block(
        content,
        start="#ifdef _WIN32\n}\n\nvoid do_nothing(void *) {",
        end="    return ctx_buf;\n  }\n#endif\n\n",
        replacement="",
        description="2b.14  remove dxgi_init + do_nothing wrapper",
    )

    # 2b.15  map_base_dev_type cleanup. Strip VAAPI, VULKAN (with its
    # #ifdef wrapper), CUDA, and VIDEOTOOLBOX cases. Keep D3D11VA, NONE,
    # default. The function's empty-line + trailing `return ::unknown;`
    # are preserved.
    content = edit(
        content,
        find=(
            "  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {\n"
            "    switch (type) {\n"
            "      case AV_HWDEVICE_TYPE_D3D11VA:\n"
            "        return platf::mem_type_e::dxgi;\n"
            "      case AV_HWDEVICE_TYPE_VAAPI:\n"
            "        return platf::mem_type_e::vaapi;\n"
            "#ifdef SUNSHINE_BUILD_VULKAN\n"
            "      case AV_HWDEVICE_TYPE_VULKAN:\n"
            "        return platf::mem_type_e::vulkan;\n"
            "#endif\n"
            "      case AV_HWDEVICE_TYPE_CUDA:\n"
            "        return platf::mem_type_e::cuda;\n"
            "      case AV_HWDEVICE_TYPE_NONE:\n"
            "        return platf::mem_type_e::system;\n"
            "      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:\n"
            "        return platf::mem_type_e::videotoolbox;\n"
            "      default:\n"
            "        return platf::mem_type_e::unknown;\n"
            "    }\n"
        ),
        replace=(
            "  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {\n"
            "    switch (type) {\n"
            "      case AV_HWDEVICE_TYPE_D3D11VA:\n"
            "        return platf::mem_type_e::dxgi;\n"
            "      case AV_HWDEVICE_TYPE_NONE:\n"
            "        return platf::mem_type_e::system;\n"
            "      default:\n"
            "        return platf::mem_type_e::unknown;\n"
            "    }\n"
        ),
        description="2b.15  prune map_base_dev_type cases to D3D11VA + NONE + default",
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

    PATH.write_bytes(content.encode("utf-8"))
    print(f"  Wrote {PATH} (LF endings).")


if __name__ == "__main__":
    main()
