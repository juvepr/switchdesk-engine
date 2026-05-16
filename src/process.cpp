/**
 * @file src/process.cpp
 * @brief Minimal launcher stub for SwitchDesk. See process.h.
 */

// standard includes
#include <algorithm>
#include <string>
#include <vector>

// local includes
#include "display_device.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"

namespace proc {
  using namespace std::literals;

  proc_t proc;

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  proc_t::proc_t() {
    _apps.push_back({"Desktop", "1"});
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> /*launch_session*/) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto &app) {
      return app.id == std::to_string(app_id);
    });
    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }
    _app_id = app_id;
    BOOST_LOG(info) << "Executing [Desktop]"sv;
    return 0;
  }

  int proc_t::running() {
    return _app_id;
  }

  void proc_t::terminate() {
    bool was_running = _app_id > 0;
    _app_id = 0;
    if (was_running) {
      display_device::revert_configuration();
    }
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  std::string proc_t::get_app_image(int /*app_id*/) {
    return DEFAULT_APP_IMAGE_PATH;
  }

  void refresh(const std::string & /*file_name*/) {
    // No-op stub. The apps.json reload pattern is gone with the launcher.
  }
}  // namespace proc
