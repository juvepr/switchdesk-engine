/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// lib includes (host-auth step 3: explicit SSL_* client-cert API)
#include <openssl/ssl.h>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "nvhttp_token.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  // Host-auth step 3: per-connection client-TLS handshake state, joined by
  // the token gate at request time. SWS keeps Request::connection private,
  // so the peer endpoint is the only key both sides can read. Written in
  // accept() after a successful handshake when verification is armed; read
  // in check_token(). A lookup MISS is a first-class verdict (NO-TLS-STATE),
  // never conflated with "no cert presented".
  namespace client_tls {
    struct info_t {
      bool cert_presented;
      bool chain_ok;  ///< meaningful only when cert_presented
      long verify_err;  ///< raw X509_V_ERR_* (X509_V_OK when chain_ok)
      std::string x5t;  ///< base64url-nopad SHA-256 of the leaf DER; empty when no cert
    };

    // Bounded registry: the cap is a DoS bound, far above any plausible
    // live-connection count (handlers close per response). Eviction takes
    // the oldest entry by insertion sequence and logs at warning -- an
    // evicted LIVE connection later verdicts NO-TLS-STATE, never a false
    // "no cert".
    constexpr std::size_t kCap = 256;
    std::mutex mutex;
    std::uint64_t seq_counter;  // guarded by mutex
    std::map<boost::asio::ip::tcp::endpoint, std::pair<std::uint64_t, info_t>> map;  // guarded by mutex

    void note_handshake(const boost::asio::ip::tcp::endpoint &peer, SSL *ssl) {
      // Local is `st`, NOT `info`: BOOST_LOG(info) resolves `info` by
      // ordinary lookup to the global severity logger (logging.h), so a
      // local named `info` would shadow it and break compilation.
      info_t st {};
      crypto::x509_t leaf {SSL_get_peer_certificate(ssl)};
      if (leaf) {
        st.cert_presented = true;
        // The context verify callback always returns 1 (the handshake must
        // complete so a policy failure can answer 401), but OpenSSL still
        // latches the REAL chain verdict here -- including
        // X509_V_ERR_CERT_HAS_EXPIRED (the optional_no_ca pattern).
        st.verify_err = SSL_get_verify_result(ssl);
        st.chain_ok = (st.verify_err == X509_V_OK);
        st.x5t = token::x5t_s256(leaf.get());
      }
      // Without a cert no verification ran -- print chain=-, never chain=ok.
      BOOST_LOG(debug) << "nvhttp: client-tls "sv << peer
                      << " cert="sv << (st.cert_presented ? "yes"sv : "no"sv)
                      << " chain="sv
                      << (!st.cert_presented ? "-"s :
                            st.chain_ok      ? "ok"s :
                                               ("FAIL("s + std::to_string(st.verify_err) + ")"s))
                      << " x5t="sv << (st.x5t.empty() ? "-"sv : std::string_view {st.x5t});
      std::lock_guard lock {mutex};
      if (map.size() >= kCap && map.find(peer) == map.end()) {
        auto oldest = std::min_element(map.begin(), map.end(), [](const auto &a, const auto &b) {
          return a.second.first < b.second.first;
        });
        // Debug, not warning: eviction is expected churn for ungated
        // (serverinfo) connections whose entry is never taken; the gated
        // path takes-on-read below, so this is not an error condition.
        BOOST_LOG(debug) << "nvhttp: client-tls registry at cap; evicting "sv << oldest->first;
        map.erase(oldest);
      }
      map[peer] = {++seq_counter, std::move(st)};
    }

    // Take-on-read: each connection serves exactly one request
    // (close_connection_after_response), so the gated request consumes its
    // own entry and the map holds only in-flight-plus-ungated state -- kCap
    // becomes a genuine concurrency bound, not an unbounded high-water mark.
    std::optional<info_t> take(const boost::asio::ip::tcp::endpoint &peer) {
      // A default-constructed endpoint means the socket could not be read
      // (peer already gone) -- that is a miss, not a key.
      if (peer == boost::asio::ip::tcp::endpoint {}) {
        return std::nullopt;
      }
      std::lock_guard lock {mutex};
      auto it = map.find(peer);
      if (it == map.end()) {
        return std::nullopt;
      }
      info_t st = std::move(it->second.second);
      map.erase(it);
      return st;
    }
  }  // namespace client_tls

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<int(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

    // Host-auth step 3: load the client-CA trust anchors and advertise the
    // acceptable CA names in the CertificateRequest. Called once from
    // nvhttp::start() (a free function -- hence public, like verify above),
    // only when client_ca_bundle is configured; a false return is a fatal
    // config failure at the caller.
    bool arm_client_ca(const std::string &bundle_path) {
      try {
        context.load_verify_file(bundle_path);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "nvhttp: client-CA bundle load failed: "sv << e.what();
        return false;
      }
      auto ca_list = SSL_load_client_CA_file(bundle_path.c_str());
      if (!ca_list) {
        BOOST_LOG(error) << "nvhttp: client-CA name list load failed: "sv << bundle_path;
        return false;
      }
      // A readable-but-zero-cert bundle yields a non-null EMPTY name
      // stack (SSL_load_client_CA_file does not fail on it) -- reject so a
      // hand-truncated bundle fatals rather than arming an empty trust set.
      if (sk_X509_NAME_num(ca_list) <= 0) {
        BOOST_LOG(error) << "nvhttp: client-CA bundle has zero certificates: "sv << bundle_path;
        sk_X509_NAME_pop_free(ca_list, X509_NAME_free);
        return false;
      }
      SSL_CTX_set_client_CA_list(context.native_handle(), ca_list);  // takes ownership
      return true;
    }

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        // BOTH advisory and enforcing modes complete the handshake --
        // gate-4 rejections are 401s, not TLS aborts ("To respond with an
        // error message, a connection must be established"). Enforcement
        // lives post-handshake in the verify hook and the token gate;
        // fail_if_no_peer_cert is deliberately never set.
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // Always continue; OpenSSL latches the real chain verdict into
          // the connection for SSL_get_verify_result.
          return 1;
        });
        // SSL_VERIFY_PEER with an empty session-id context FATALs TLS<=1.2
        // resumption (SSL_R_SESSION_ID_CONTEXT_UNINITIALIZED) instead of
        // falling back to a full handshake; TLS 1.3 PSK resumption hides
        // this on modern benches. Mirror the vendored SWS derivation
        // (server_https.hpp after_bind).
        auto session_id_context = std::to_string(acceptor->local_endpoint().port()) + ':';
        session_id_context.append(config.address.rbegin(), config.address.rend());
        SSL_CTX_set_session_id_context(context.native_handle(),
                                       reinterpret_cast<const unsigned char *>(session_id_context.data()),
                                       static_cast<unsigned int>(std::min<std::size_t>(session_id_context.size(), SSL_MAX_SSL_SESSION_ID_LENGTH)));
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify) {
                // Host-auth step 3: record this connection's client-TLS
                // state for the request-time cnf join. Endpoint read via
                // the error_code overload; an unreadable/default endpoint
                // is skipped (its requests verdict as NO-TLS-STATE).
                boost::system::error_code ep_ec;
                auto peer = session->connection->socket->lowest_layer().remote_endpoint(ep_ec);
                if (!ep_ec && peer != boost::asio::ip::tcp::endpoint {}) {
                  client_tls::note_handshake(peer, session->connection->socket->native_handle());
                }
              }
              if (verify && !verify(session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;
    pt::write_xml(data, tree);

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/xml");
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str(), headers);
    response->close_connection_after_response = true;
  }

  // Phase 2 token gate. Each authenticated handler calls check_token() at
  // the top after print_req. On missing/invalid token, write_401 emits a
  // 401 with a small XML body and the handler returns immediately. Gated:
  // /applist, /launch, /resume, /cancel, /appasset. Ungated: /serverinfo
  // (discovery — must work before any session exists). /pair removed.
  void write_401(resp_https_t response, const std::string &message) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 401);
    tree.put("root.<xmlattr>.status_message", message);

    std::ostringstream data;
    pt::write_xml(data, tree);

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/xml");
    response->write(SimpleWeb::StatusCode::client_error_unauthorized, data.str(), headers);
    response->close_connection_after_response = true;
  }

  bool check_token(resp_https_t response, req_https_t request) {
    const auto args = request->parse_query_string();
    const auto token_it = args.find("token");
    if (token_it == std::end(args)) {
      BOOST_LOG(debug) << "nvhttp: missing token on "sv << request->path;
      write_401(response, "Missing token");
      return false;
    }
    const auto claims = nvhttp::token::verify(token_it->second);
    if (!claims) {
      BOOST_LOG(debug) << "nvhttp: invalid token on "sv << request->path;
      write_401(response, "Invalid token");
      return false;
    }
    BOOST_LOG(debug) << "nvhttp: authenticated sid="sv << claims->sid << " path="sv << request->path;

    // Host-auth step 3: join the token's cnf with the connection's client-
    // TLS state. Advisory (default): one info verdict line per gated
    // request -- the engine leg of the cross-component x5t match chain
    // (the presented leaf's thumbprint == the token's cnf claim).
    // cnf_enforce (step 4): 401 on any verdict other than a clean MATCH;
    // NO-TLS-STATE is fail-closed as its own class so registry loss is
    // never misattributed to the client.
    if (config::nvhttp.client_ca_bundle.empty()) {
      // Disarmed engine; the CP mints cnf regardless (rollout/rollback
      // state). Debug, not info: expected noise, not acceptance evidence.
      BOOST_LOG(debug) << "nvhttp: client-tls sid="sv << claims->sid << " cnf=DISARMED"sv;
      return true;
    }
    const auto tls = client_tls::take(request->remote_endpoint());
    std::string verdict;
    bool cnf_ok = false;
    if (!tls) {
      verdict = "NO-TLS-STATE";
    } else if (!claims->cnf_x5t) {
      verdict = "ABSENT";
    } else if (!tls->cert_presented) {
      verdict = "NO-CLIENT-CERT tok=" + *claims->cnf_x5t;
    } else if (*claims->cnf_x5t == tls->x5t) {
      // Belt: enforcing also requires the chain here, though the policy
      // hook already 401'd bad chains before any request was read.
      cnf_ok = tls->chain_ok;
      verdict = "MATCH x5t=" + tls->x5t + (tls->chain_ok ? "" : " (chain FAILED)");
    } else {
      verdict = "MISMATCH tok=" + *claims->cnf_x5t + " presented=" + tls->x5t;
    }
    // Cert/chain evidence rides the post-auth verdict line (info); the
    // handshake line above is debug so an unauthenticated peer cannot
    // flood the info log by connecting.
    std::string chain_field;
    if (tls && tls->cert_presented) {
      chain_field = tls->chain_ok ? " chain=ok"s : (" chain=FAIL("s + std::to_string(tls->verify_err) + ")"s);
    } else if (tls) {
      chain_field = " cert=no"s;
    }
    BOOST_LOG(info) << "nvhttp: client-tls sid="sv << claims->sid << " path="sv << request->path << chain_field << " cnf="sv << verdict;
    if (config::nvhttp.cnf_enforce && !cnf_ok) {
      write_401(response, "Client certificate binding failed");
      return false;
    }
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    constexpr int pair_status = 1;

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (!check_token(response, request)) {
      return;
    }

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (!check_token(response, request)) {
      return;
    }

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args);

    if (rtsp_stream::session_count() == 0) {
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (!check_token(response, request)) {
      return;
    }

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args);

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (!check_token(response, request)) {
      return;
    }

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (!check_token(response, request)) {
      return;
    }

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // Phase 2: load the control plane issuer pubkey for JWT verification.
    // If cp_pubkey is empty, default to a sibling cp-pubkey.pem next to the
    // server cert. Fatal failure inside load_issuer_pubkey raises shutdown.
    std::string cp_pubkey_path = config::nvhttp.cp_pubkey;
    if (cp_pubkey_path.empty()) {
      cp_pubkey_path = file_handler::get_parent_directory(config::nvhttp.cert) + "/cp-pubkey.pem";
    }
    nvhttp::token::load_issuer_pubkey(cp_pubkey_path);

    // Host-auth step 3: client-CA trust material. Set-but-broken is FATAL
    // (a broken security config must never silently disable); empty is
    // cleanly disarmed (pre-step-3 TLS behavior). Resolution mirrors
    // path_f (appdata-relative) minus its empty-input trap -- the same
    // reason client_ca_bundle parses via string_f.
    std::string client_ca_path = config::nvhttp.client_ca_bundle;
    if (!client_ca_path.empty()) {
      fs::path resolved = client_ca_path;
      if (resolved.is_relative()) {
        resolved = platf::appdata() / resolved;
      }
      client_ca_path = resolved.string();
      if (!fs::exists(resolved)) {
        BOOST_LOG(fatal) << "nvhttp: client_ca_bundle set but missing: "sv << client_ca_path;
        shutdown_event->raise(true);
        return;
      }
    } else if (config::nvhttp.cnf_enforce) {
      BOOST_LOG(fatal) << "nvhttp: cnf_enforce set without client_ca_bundle -- refusing to enforce without trust material"sv;
      shutdown_event->raise(true);
      return;
    }

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Host-auth step 3: arm client-cert verification. Advisory by default;
    // cnf_enforce flips exactly two policy returns (here and in the token
    // gate) -- the step-4 config flip.
    if (!client_ca_path.empty()) {
      if (!https_server.arm_client_ca(client_ca_path)) {
        BOOST_LOG(fatal) << "nvhttp: client-CA arming failed: "sv << client_ca_path;
        shutdown_event->raise(true);
        return;
      }
      https_server.verify = [](SSL *ssl) -> int {
        if (!config::nvhttp.cnf_enforce) {
          return 1;  // advisory: verdicts are logged (note_handshake), never enforced
        }
        crypto::x509_t leaf {SSL_get_peer_certificate(ssl)};
        if (!leaf) {
          return 0;  // step 4: no client cert -> 401 via on_verify_failed
        }
        return SSL_get_verify_result(ssl) == X509_V_OK ? 1 : 0;  // bad chain / expired -> 401
      };
      https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
        write_401(resp, "Client certificate required");
      };
      BOOST_LOG(info) << "nvhttp: client-cert verification armed ("sv
                      << (config::nvhttp.cnf_enforce ? "ENFORCING"sv : "advisory"sv)
                      << "), bundle "sv << client_ca_path;
    }

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }
}  // namespace nvhttp
