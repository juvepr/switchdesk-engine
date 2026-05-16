/**
 * @file src/process.h
 * @brief Minimal launcher stub for SwitchDesk.
 *
 * The Moonlight protocol requires /launch, /resume, /cancel, /applist, and
 * /appasset endpoints in nvhttp.cpp that consult proc_t for app lifecycle
 * state. SwitchDesk's session model is "the desktop is the only app", so
 * proc_t carries no real launcher machinery: it exposes a single hard-coded
 * "Desktop" entry, tracks a live-session flag, and lets the engine pass
 * Moonlight handshakes. The boost::process spawning, apps.json parsing, and
 * prep_cmd / undo_cmd plumbing of upstream Sunshine are gone.
 */
#pragma once

// standard includes
#include <memory>
#include <string>
#include <vector>

// local includes
#include "platform/common.h"
#include "rtsp.h"

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * Minimal app context. SwitchDesk exposes a single "Desktop" app.
   */
  struct ctx_t {
    std::string name;
    std::string id;
  };

  class proc_t {
  public:
    proc_t();
    ~proc_t() = default;

    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a session is live, otherwise `0`.
     */
    int running();

    void terminate();

    const std::vector<ctx_t> &get_apps() const;
    std::vector<ctx_t> &get_apps();
    std::string get_app_image(int app_id);

  private:
    int _app_id {0};
    std::vector<ctx_t> _apps;
  };

  /**
   * @brief No-op stub. Retained for the main.cpp call site that used to
   *        reload apps.json from disk.
   */
  void refresh(const std::string &file_name);

  /**
   * @brief Initialize proc. Returns a deinit_t that calls terminate() on shutdown.
   */
  std::unique_ptr<platf::deinit_t> init();

  extern proc_t proc;
}  // namespace proc
