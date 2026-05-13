#!/usr/bin/env python3
"""
Phase 1 step 4 commit 2: cut d3d_avcodec_encode_device_t and its orbit.

The encoder-side avcodec session machinery was cut in step 2. This is the
display-side companion: the avcodec encode-device class that bridged
display capture to the (now-removed) avcodec encoders. Post-cut, only
the NVENC encode-device path remains.

Caller at video.cpp:783 (gated by dynamic_cast<encoder_platform_formats_avcodec*>)
is left in place — that dynamic_cast already returns null post-step-2 since
no surviving encoder has encoder_platform_formats_avcodec. The probing
region as a whole is step 8's territory.

Files touched:
  src/platform/windows/display_vram.cpp
    - REMOVE extern "C" { #include <libavcodec/avcodec.h>
                         #include <libavutil/hwcontext_d3d11va.h> }
      (no remaining users in this file after the cut)
    - REMOVE free_frame() and `using frame_t = ...` helpers at file scope
      (only used inside d3d_avcodec_encode_device_t; orphan after cut)
    - REMOVE d3d_avcodec_encode_device_t class (~89 lines)
    - REMOVE display_vram_t::make_avcodec_encode_device() factory (7 lines)
  src/platform/windows/display.h
    - REMOVE make_avcodec_encode_device declaration in display_vram_t
      (linker requires definition to match declaration; falls through to
       the no-op base class default in common.h:508 which is never called
       post-step-2)

Idempotent: re-running is safe. Same template as cut-wgc.py.

Usage:
    python3 cut-d3d-avcodec-encode-device.py /path/to/switchdesk-engine
"""

import re
import sys
from pathlib import Path

REPO = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()


# ----- I/O helpers (CRLF-aware read, LF write) -----

def read_lf(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def write_lf(path, content):
    path.write_bytes(content.encode("utf-8"))


# ----- Edit primitives (idempotent) -----

def delete_block(path, anchor, label):
    if not path.exists():
        print(f"  skip: {label} (file does not exist)")
        return False
    content = read_lf(path)
    count = content.count(anchor)
    if count == 0:
        print(f"  skip: {label} (already removed)")
        return False
    if count > 1:
        raise SystemExit(
            f"FAIL: {label} matched {count} times in {path}; anchor not unique"
        )
    write_lf(path, content.replace(anchor, "", 1))
    print(f"  cut:  {label} ({len(anchor)} chars)")
    return True


def replace_unique(path, old, new, label):
    if not path.exists():
        print(f"  skip: {label} (file does not exist)")
        return False
    content = read_lf(path)
    if old not in content:
        if new in content:
            print(f"  skip: {label} (already replaced)")
            return False
        raise SystemExit(f"FAIL: {label} anchor not found in {path}")
    if content.count(old) > 1:
        raise SystemExit(
            f"FAIL: {label} old text matched {content.count(old)} times; not unique"
        )
    write_lf(path, content.replace(old, new, 1))
    print(f"  edit: {label}")
    return True


def collapse_blank_lines(path):
    if not path.exists():
        return
    content = read_lf(path)
    new_content, n = re.subn(r"\n{3,}", "\n\n", content)
    if n:
        write_lf(path, new_content)
        print(f"  fmt:  {path.relative_to(REPO)} collapsed {n} multi-blank-line run(s)")


# ============================================================
# 1. remove extern "C" avcodec includes block (display_vram.cpp)
# ============================================================

print("[1/5] display_vram.cpp: extern \"C\" avcodec/avutil includes")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    """extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

""",
    "extern \"C\" { libavcodec + hwcontext_d3d11va } block",
)


# ============================================================
# 2. remove free_frame() and `using frame_t = ...` orphan helpers
# ============================================================

print("[2/5] display_vram.cpp: free_frame() + frame_t alias")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    """static void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

using frame_t = util::safe_ptr<AVFrame, free_frame>;

""",
    "free_frame() + frame_t alias (orphan after class cut)",
)


# ============================================================
# 3. remove d3d_avcodec_encode_device_t class
# ============================================================

print("[3/5] display_vram.cpp: d3d_avcodec_encode_device_t class")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    """  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      int result = base.init(display, adapter_p, pix_fmt);
      data = base.device.get();
      return result;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

    void apply_colorspace() override {
      base.apply_colorspace(colorspace);
    }

    void init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    int prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame {av_frame_alloc()};

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      } else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) frame->data[0];
      }

      return base.init_output(frame_texture, frame->width, frame->height);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

""",
    "d3d_avcodec_encode_device_t class body",
)


# ============================================================
# 4. remove display_vram_t::make_avcodec_encode_device() factory
# ============================================================

print("[4/5] display_vram.cpp: display_vram_t::make_avcodec_encode_device()")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    """  std::unique_ptr<avcodec_encode_device_t> display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

""",
    "display_vram_t::make_avcodec_encode_device() factory",
)


# ============================================================
# 5. remove the declaration in display.h
# ============================================================

print("[5/5] display.h: display_vram_t::make_avcodec_encode_device declaration")
# Anchor uniqueness: display_ram_t also has a make_avcodec_encode_device
# declaration with identical text. Disambiguate by anchoring on the
# adjacent make_nvenc_encode_device line, which appears only in
# display_vram_t. Replace both lines with just the make_nvenc line —
# effectively deleting only the avcodec line.
replace_unique(
    REPO / "src/platform/windows/display.h",
    """    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) override;
""",
    """    std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) override;
""",
    "make_avcodec_encode_device override declaration in display_vram_t",
)


# ============================================================
# post-pass: blank-line collapse
# ============================================================

print("[fmt] blank-line collapse")
for relpath in [
    "src/platform/windows/display.h",
    "src/platform/windows/display_vram.cpp",
]:
    collapse_blank_lines(REPO / relpath)

print("done.")
