#!/usr/bin/env python3
"""
Phase 1 step 4 commit 3: collapse is_codec_supported() to NVIDIA-only.

The remaining target hardware after Phase 1 is NVIDIA Turing or newer
(GTX 1660 Ti on sd-kc-01, RTX 3060 Ti on the dev box). The pre-cut
is_codec_supported() body branched on VendorId (AMD 0x1002, Intel 0x8086,
NVIDIA 0x10de, Qualcomm 0x4D4F4351/0x5143) with vendor-specific encoder
suffix checks plus AMF version checks for AMD. With the NVENC-only
hardware target, the entire non-NVIDIA branch tree is dead weight.

Replace the body with: reject non-NVIDIA adapters with a clear error;
reject non-NVENC encoder names; accept everything else.

Opportunistic cleanup: the <AMF/core/Factory.h> include becomes orphan
once the AMD branch is gone (tree-wide grep confirmed AMF symbols
appear only inside the AMD branch — config.cpp's AMF block is unrelated
encoder config constants, not Factory.h usage).

Note: is_codec_supported is called from probing code in video.cpp
(probe_encoders, validate_encoder, validate_config) which step 8 will
gut. Don't delete the function — simplify it.

Files touched:
  src/platform/windows/display_vram.cpp
    - REMOVE #include <AMF/core/Factory.h>
    - REPLACE is_codec_supported() body with NVIDIA-only version

Usage:
    python3 cut-non-nvidia-vendors.py /path/to/switchdesk-engine
"""

import re
import sys
from pathlib import Path

REPO = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()


def read_lf(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def write_lf(path, content):
    path.write_bytes(content.encode("utf-8"))


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
# 1. remove AMF include (orphan once AMD branch is gone below)
# ============================================================

print("[1/2] display_vram.cpp: <AMF/core/Factory.h> include")
delete_block(
    REPO / "src/platform/windows/display_vram.cpp",
    "#include <AMF/core/Factory.h>\n",
    "<AMF/core/Factory.h> include (orphan after AMD branch cut)",
)


# ============================================================
# 2. collapse is_codec_supported() to NVIDIA-only
# ============================================================

print("[2/2] display_vram.cpp: is_codec_supported() body — NVIDIA-only")
replace_unique(
    REPO / "src/platform/windows/display_vram.cpp",
    """  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      // If it's not an AMF encoder, it's not compatible with an AMD GPU
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            } else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          } else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        } else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      } else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    } else if (adapter_desc.VendorId == 0x8086) {  // Intel
      // If it's not a QSV encoder, it's not compatible with an Intel GPU
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV doesn't support 4:4:4 in H.264 or AV1
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    } else if (adapter_desc.VendorId == 0x10de) {  // Nvidia
      // If it's not an NVENC encoder, it's not compatible with an Nvidia GPU
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    } else if (adapter_desc.VendorId == 0x4D4F4351 ||  // Qualcomm (QCOM as MOQC reversed)
               adapter_desc.VendorId == 0x5143) {  // Qualcomm alternate ID
      // If it's not a MediaFoundation encoder, it's not compatible with a Qualcomm GPU
      if (!boost::algorithm::ends_with(name, "_mf")) {
        return false;
      }
    } else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }
""",
    """  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    // SwitchDesk requires NVIDIA Turing or newer (NVENC-only). Reject
    // non-NVIDIA adapters and non-NVENC encoder names.
    if (adapter_desc.VendorId != 0x10de) {
      BOOST_LOG(error) << "SwitchDesk requires an NVIDIA GPU (got VendorId 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << ")"sv;
      return false;
    }

    if (!boost::algorithm::ends_with(name, "_nvenc")) {
      return false;
    }

    return true;
  }
""",
    "is_codec_supported() body — NVIDIA-only",
)


# ============================================================
# post-pass: blank-line collapse
# ============================================================

print("[fmt] blank-line collapse")
collapse_blank_lines(REPO / "src/platform/windows/display_vram.cpp")

print("done.")
