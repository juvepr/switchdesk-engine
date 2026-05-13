#!/usr/bin/env python3
"""
Phase 1 step 4 commit 1: cut WGC (Windows.Graphics.Capture) entirely.

Executes the "DXGI Desktop Duplication-only" architectural decision
captured in decisions.md 2026-05-10. WGC requires an interactive session;
the streaming engine runs as a SYSTEM service.

Touches five files:
  - DELETES src/platform/windows/display_wgc.cpp (344 lines, entirely WGC)
  - REMOVES one line from cmake/compile_definitions/windows.cmake
  - REMOVES three class declarations from src/platform/windows/display.h
    (wgc_capture_t, display_wgc_ram_t, display_wgc_vram_t)
  - REMOVES three method bodies from src/platform/windows/display_vram.cpp
    (display_wgc_vram_t::{snapshot, release_snapshot, init})
  - REMOVES factory branch from src/platform/windows/display_base.cpp
    and updates the trailing "ddx and wgc failed" comment

Idempotent: re-running is safe. Each block matched by literal text;
unique match required; missing match means already cut.

Usage:
    python3 cut-wgc.py /path/to/switchdesk-engine
"""

import re
import sys
from pathlib import Path

REPO = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()


# ----- I/O helpers (CRLF-aware read, LF write) -----

def read_lf(path):
    """Read file as text, normalizing CRLF→LF for matching."""
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def write_lf(path, content):
    """Write file with LF endings (matches this repo's git index encoding)."""
    path.write_bytes(content.encode("utf-8"))


# ----- Edit primitives (idempotent) -----

def delete_block(path, anchor, label):
    """Delete an exact literal block. Skip if already gone; fail if non-unique."""
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
    """Replace an exact literal substring. Skip if already replaced; fail if non-unique."""
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


def delete_file(path, label):
    """Delete a file. Idempotent."""
    if not path.exists():
        print(f"  skip: {label} (already deleted)")
        return False
    path.unlink()
    print(f"  del:  {label}")
    return True


def collapse_blank_lines(path):
    """Collapse runs of 3+ newlines down to 2 (one blank line)."""
    if not path.exists():
        return
    content = read_lf(path)
    new_content, n = re.subn(r"\n{3,}", "\n\n", content)
    if n:
        write_lf(path, new_content)
        print(f"  fmt:  {path.relative_to(REPO)} collapsed {n} multi-blank-line run(s)")


# ============================================================
# 1. delete display_wgc.cpp entirely
# ============================================================

print("[1/5] display_wgc.cpp")
delete_file(
    REPO / "src/platform/windows/display_wgc.cpp",
    "src/platform/windows/display_wgc.cpp",
)


# ============================================================
# 2. remove the line from cmake/compile_definitions/windows.cmake
# ============================================================

print("[2/5] cmake/compile_definitions/windows.cmake")
delete_block(
    REPO / "cmake/compile_definitions/windows.cmake",
    '        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"\n',
    "display_wgc.cpp from PLATFORM_TARGET_FILES",
)


# ============================================================
# 3. remove three WGC class declarations from display.h
# ============================================================

print("[3/5] display.h: wgc_capture_t + display_wgc_ram_t + display_wgc_vram_t")
delete_block(
    REPO / "src/platform/windows/display.h",
    """  /**
   * Display duplicator that uses the Windows.Graphics.Capture API.
   */
  class wgc_capture_t {
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice uwp_device {nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item {nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool {nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture_session {nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame produced_frame {nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame consumed_frame {nullptr};
    SRWLOCK frame_lock = SRWLOCK_INIT;
    CONDITION_VARIABLE frame_present_cv;

    void on_frame_arrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &);

  public:
    wgc_capture_t();
    ~wgc_capture_t();

    int init(display_base_t *display, const ::video::config_t &config);
    capture_e next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time);
    capture_e release_frame();
    int set_cursor_visible(bool);
  };

  /**
   * Display backend that uses Windows.Graphics.Capture with a software encoder.
   */
  class display_wgc_ram_t: public display_ram_t {
    wgc_capture_t dup;

  public:
    int init(const ::video::config_t &config, const std::string &display_name);
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e release_snapshot() override;
  };

  /**
   * Display backend that uses Windows.Graphics.Capture with a hardware encoder.
   */
  class display_wgc_vram_t: public display_vram_t {
    wgc_capture_t dup;

  public:
    int init(const ::video::config_t &config, const std::string &display_name);
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e release_snapshot() override;
  };
""",
    "three WGC class declarations (wgc_capture_t + display_wgc_{ram,vram}_t)",
)


# ============================================================
# 4. remove display_wgc_vram_t method implementations from display_vram.cpp
# ============================================================

print("[4/5] display_vram.cpp: display_wgc_vram_t::{snapshot, release_snapshot, init}")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    """  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible
   */
  capture_e display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
      } else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::error;
      }
    } else {
      return capture_e::error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

""",
    "display_wgc_vram_t three method bodies",
)


# ============================================================
# 5. remove WGC factory branch from display_base.cpp + comment fix
# ============================================================

print("[5/5] display_base.cpp: WGC factory branch + comment")
delete_block(
    REPO / "src/platform/windows/display_base.cpp",
    """    if (config::video.capture == "wgc" || config::video.capture.empty()) {
      if (hwdevice_type == mem_type_e::dxgi) {
        auto disp = std::make_shared<dxgi::display_wgc_vram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      } else if (hwdevice_type == mem_type_e::system) {
        auto disp = std::make_shared<dxgi::display_wgc_ram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      }
    }

""",
    "WGC factory branch in display() factory function",
)
replace_unique(
    REPO / "src/platform/windows/display_base.cpp",
    "    // ddx and wgc failed",
    "    // ddx failed",
    "comment: 'ddx and wgc failed' → 'ddx failed'",
)


# ============================================================
# post-pass: blank-line collapse
# ============================================================

print("[fmt] blank-line collapse")
for relpath in [
    "src/platform/windows/display.h",
    "src/platform/windows/display_vram.cpp",
    "src/platform/windows/display_base.cpp",
    "cmake/compile_definitions/windows.cmake",
]:
    collapse_blank_lines(REPO / relpath)

print("done.")
