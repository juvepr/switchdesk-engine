/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <string>

// lib includes
#include <Simple-Web-Server/server_https.hpp>

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Gracefully shutdown the TLS connection
      SimpleWeb::error_code ec;
      shutdown(ec);
    }
  };

}  // namespace nvhttp
