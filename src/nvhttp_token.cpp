/**
 * @file src/nvhttp_token.cpp
 * @brief JWT (RS256) verifier for SwitchDesk Phase 2 token-based session start.
 *
 * Design: docs/phase-2-design.md in juvepr/switchdesk.
 *
 * Verify path (in order):
 *   1. Split on '.' — exactly 3 parts.
 *   2. Decode + parse header; check alg == "RS256" STRICTLY (before any
 *      signature work). Closes alg-substitution attacks.
 *   3. Decode signature.
 *   4. Reconstruct signing input as the literal "header.payload" wire
 *      bytes (no re-encoding).
 *   5. Verify RS256 signature via crypto::verify256(x509_t, ...).
 *   6. Decode + parse payload JSON.
 *   7. Validate claims: exp > now, iat <= now + 60s clock skew,
 *      iss == config::nvhttp.cp_issuer, aud == config::nvhttp.node_id.
 *
 * Failures at any step return std::nullopt; the caller writes a generic
 * 401 with no leak of which check failed.
 */
// standard includes
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "globals.h"
#include "logging.h"
#include "nvhttp_token.h"

using namespace std::literals;

namespace nvhttp::token {

  namespace {

    /**
     * @brief The control-plane issuer public key, loaded once at
     *        nvhttp::start(). All verify() paths fail closed (return
     *        nullopt) if this is not populated.
     */
    crypto::x509_t issuer_pubkey;

    /**
     * @brief Clock-skew tolerance for the iat ("issued-at") check, in
     *        seconds. A token whose iat is more than this far in the
     *        future (per the engine's clock) is rejected. exp is checked
     *        without skew — an expired token is expired.
     */
    constexpr int64_t kClockSkewSeconds = 60;

    /**
     * @brief Base64url-decode a string into a byte vector.
     *
     * Implements RFC 4648 §5 base64url. Tolerant of optional trailing
     * '=' padding (accepts with or without). Rejects any character
     * outside [A-Za-z0-9-_=] and rejects a 1-char tail (always invalid
     * for base64).
     *
     * @param input  Base64url-encoded text.
     * @param out    Destination byte buffer; cleared and overwritten.
     * @return true on success, false on any invalid character or
     *         malformed length.
     */
    bool base64url_decode(std::string_view input, std::vector<uint8_t> &out) {
      out.clear();
      out.reserve((input.size() * 3) / 4);

      auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
          return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
          return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
          return c - '0' + 52;
        }
        if (c == '-') {
          return 62;
        }
        if (c == '_') {
          return 63;
        }
        return -1;
      };

      // Strip any trailing padding. Standard JWTs omit padding, but tolerate
      // it if a producer adds it.
      while (!input.empty() && input.back() == '=') {
        input.remove_suffix(1);
      }

      // Process 4 input chars → 3 output bytes at a time.
      size_t i = 0;
      while (i + 4 <= input.size()) {
        const int a = decode_char(input[i]);
        const int b = decode_char(input[i + 1]);
        const int c = decode_char(input[i + 2]);
        const int d = decode_char(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
          return false;
        }
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        out.push_back(static_cast<uint8_t>(((b & 0x0F) << 4) | (c >> 2)));
        out.push_back(static_cast<uint8_t>(((c & 0x03) << 6) | d));
        i += 4;
      }

      // Handle the tail. Valid tail lengths: 0, 2, or 3 chars. 1 is invalid.
      const size_t tail = input.size() - i;
      if (tail == 1) {
        return false;
      }
      if (tail == 2) {
        const int a = decode_char(input[i]);
        const int b = decode_char(input[i + 1]);
        if (a < 0 || b < 0) {
          return false;
        }
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
      } else if (tail == 3) {
        const int a = decode_char(input[i]);
        const int b = decode_char(input[i + 1]);
        const int c = decode_char(input[i + 2]);
        if (a < 0 || b < 0 || c < 0) {
          return false;
        }
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        out.push_back(static_cast<uint8_t>(((b & 0x0F) << 4) | (c >> 2)));
      }
      return true;
    }

  }  // namespace

  void load_issuer_pubkey(const std::string &pem_path) {
    auto pem = file_handler::read_file(pem_path.c_str());
    if (pem.empty()) {
      BOOST_LOG(fatal) << "nvhttp::token: failed to read control plane issuer pubkey at "sv << pem_path;
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      shutdown_event->raise(true);
      return;
    }

    auto cert = crypto::x509(pem);
    if (!cert) {
      BOOST_LOG(fatal) << "nvhttp::token: failed to parse "sv << pem_path
                       << " (not a valid PEM X.509 certificate)"sv;
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      shutdown_event->raise(true);
      return;
    }

    issuer_pubkey = std::move(cert);
    BOOST_LOG(info) << "nvhttp::token: loaded control plane issuer pubkey from "sv << pem_path;
  }

  std::optional<claims_t> verify(const std::string &token, int64_t now) {
    // Fail closed if load_issuer_pubkey hasn't run successfully.
    if (!issuer_pubkey) {
      BOOST_LOG(error) << "nvhttp::token::verify called before load_issuer_pubkey succeeded"sv;
      return std::nullopt;
    }

    // 1. Split on '.' — must be exactly 3 parts.
    const auto first_dot = token.find('.');
    if (first_dot == std::string::npos) {
      BOOST_LOG(debug) << "nvhttp::token: malformed (no first dot)"sv;
      return std::nullopt;
    }
    const auto second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
      BOOST_LOG(debug) << "nvhttp::token: malformed (no second dot)"sv;
      return std::nullopt;
    }
    if (token.find('.', second_dot + 1) != std::string::npos) {
      BOOST_LOG(debug) << "nvhttp::token: malformed (more than 3 parts)"sv;
      return std::nullopt;
    }

    const std::string_view header_b64 {token.data(), first_dot};
    const std::string_view payload_b64 {token.data() + first_dot + 1, second_dot - first_dot - 1};
    const std::string_view signature_b64 {token.data() + second_dot + 1, token.size() - second_dot - 1};

    // 2. Decode + parse header. STRICT alg check happens BEFORE any
    //    signature verification work — this is what closes the
    //    alg-substitution attack family (alg=none with empty signature,
    //    alg=HS256 with pubkey-bytes-as-HMAC-key).
    std::vector<uint8_t> header_bytes;
    if (!base64url_decode(header_b64, header_bytes)) {
      BOOST_LOG(debug) << "nvhttp::token: header base64url decode failed"sv;
      return std::nullopt;
    }
    nlohmann::json header;
    try {
      header = nlohmann::json::parse(header_bytes.begin(), header_bytes.end());
    } catch (const nlohmann::json::exception &) {
      BOOST_LOG(debug) << "nvhttp::token: header JSON parse failed"sv;
      return std::nullopt;
    }
    if (!header.is_object()) {
      BOOST_LOG(debug) << "nvhttp::token: header not a JSON object"sv;
      return std::nullopt;
    }

    // alg MUST be present and MUST be the literal string "RS256".
    const auto alg_it = header.find("alg");
    if (alg_it == header.end() || !alg_it->is_string() || alg_it->get<std::string>() != "RS256") {
      BOOST_LOG(debug) << "nvhttp::token: alg not RS256"sv;
      return std::nullopt;
    }
    // typ is optional; if present must be "JWT".
    const auto typ_it = header.find("typ");
    if (typ_it != header.end()) {
      if (!typ_it->is_string() || typ_it->get<std::string>() != "JWT") {
        BOOST_LOG(debug) << "nvhttp::token: typ present but not JWT"sv;
        return std::nullopt;
      }
    }

    // 3. Decode signature.
    std::vector<uint8_t> signature_bytes;
    if (!base64url_decode(signature_b64, signature_bytes) || signature_bytes.empty()) {
      BOOST_LOG(debug) << "nvhttp::token: signature base64url decode failed"sv;
      return std::nullopt;
    }

    // 4. Reconstruct signing input as the literal "header_b64.payload_b64"
    //    bytes received on the wire. Re-encoding would change the byte
    //    representation (different padding, different equivalents) and
    //    break the signature check.
    const std::string_view signing_input {token.data(), second_dot};

    // 5. RS256 verify. crypto::verify256 wraps EVP_DigestVerify* internally.
    const std::string_view sig_view {
      reinterpret_cast<const char *>(signature_bytes.data()),
      signature_bytes.size()
    };
    if (!crypto::verify256(issuer_pubkey, signing_input, sig_view)) {
      BOOST_LOG(debug) << "nvhttp::token: signature verification failed"sv;
      return std::nullopt;
    }

    // 6. Signature is authentic — now safe to spend JSON-parse time on
    //    the payload. (Doing this AFTER signature verify limits CPU
    //    spend on unauthenticated input.)
    std::vector<uint8_t> payload_bytes;
    if (!base64url_decode(payload_b64, payload_bytes)) {
      BOOST_LOG(debug) << "nvhttp::token: payload base64url decode failed"sv;
      return std::nullopt;
    }
    nlohmann::json payload;
    try {
      payload = nlohmann::json::parse(payload_bytes.begin(), payload_bytes.end());
    } catch (const nlohmann::json::exception &) {
      BOOST_LOG(debug) << "nvhttp::token: payload JSON parse failed"sv;
      return std::nullopt;
    }
    if (!payload.is_object()) {
      BOOST_LOG(debug) << "nvhttp::token: payload not a JSON object"sv;
      return std::nullopt;
    }

    // 7. Extract required claims. nlohmann::json::at throws on missing
    //    keys; get<T>() throws on wrong type. Both caught as the same
    //    "malformed required claim" failure.
    claims_t out;
    try {
      out.iss = payload.at("iss").get<std::string>();
      out.sub = payload.at("sub").get<std::string>();
      out.aud = payload.at("aud").get<std::string>();
      out.sid = payload.at("sid").get<std::string>();
      out.iat = payload.at("iat").get<int64_t>();
      out.exp = payload.at("exp").get<int64_t>();
    } catch (const nlohmann::json::exception &) {
      BOOST_LOG(debug) << "nvhttp::token: missing or malformed required claim"sv;
      return std::nullopt;
    }

    // Host-auth step 3: optional cnf (RFC 8705 §3.1), STRICT when present
    // (the typ precedent): a signed token carrying a malformed cnf is an
    // invalid token, not an ignorable unknown. The CP is the only minter
    // and never mints malformed. Canonical form only -- exactly 43
    // base64url chars, no padding, decoding to 32 bytes (SHA-256).
    if (auto cnf_it = payload.find("cnf"); cnf_it != payload.end()) {
      if (!cnf_it->is_object()) {
        BOOST_LOG(debug) << "nvhttp::token: cnf present but not an object"sv;
        return std::nullopt;
      }
      auto x5t_it = cnf_it->find("x5t#S256");
      if (x5t_it == cnf_it->end() || !x5t_it->is_string()) {
        BOOST_LOG(debug) << "nvhttp::token: cnf missing a string x5t#S256"sv;
        return std::nullopt;
      }
      auto x5t = x5t_it->get<std::string>();
      std::vector<uint8_t> x5t_bytes;
      if (x5t.size() != 43 || !base64url_decode(x5t, x5t_bytes) || x5t_bytes.size() != 32) {
        BOOST_LOG(debug) << "nvhttp::token: cnf x5t#S256 is not canonical base64url SHA-256"sv;
        return std::nullopt;
      }
      out.cnf_x5t = std::move(x5t);
    }

    // Claim validation. Each check fail-closes with a distinct debug
    // line for triage; the caller still sees a single "invalid" outcome.
    if (out.exp <= now) {
      BOOST_LOG(debug) << "nvhttp::token: expired (exp="sv << out.exp << ", now="sv << now << ")"sv;
      return std::nullopt;
    }
    if (out.iat > now + kClockSkewSeconds) {
      BOOST_LOG(debug) << "nvhttp::token: iat too far in future (iat="sv << out.iat << ", now="sv << now << ")"sv;
      return std::nullopt;
    }
    if (out.iss != config::nvhttp.cp_issuer) {
      BOOST_LOG(debug) << "nvhttp::token: issuer mismatch"sv;
      return std::nullopt;
    }
    if (out.aud != config::nvhttp.node_id) {
      BOOST_LOG(debug) << "nvhttp::token: audience mismatch"sv;
      return std::nullopt;
    }

    return out;
  }

  std::optional<claims_t> verify(const std::string &token) {
    return verify(token, static_cast<int64_t>(std::time(nullptr)));
  }

  std::string base64url_encode(const std::uint8_t *data, std::size_t len) {
    static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
      const uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
      out.push_back(alphabet[(v >> 18) & 0x3F]);
      out.push_back(alphabet[(v >> 12) & 0x3F]);
      out.push_back(alphabet[(v >> 6) & 0x3F]);
      out.push_back(alphabet[v & 0x3F]);
    }
    if (i + 1 == len) {
      const uint32_t v = data[i] << 16;
      out.push_back(alphabet[(v >> 18) & 0x3F]);
      out.push_back(alphabet[(v >> 12) & 0x3F]);
    } else if (i + 2 == len) {
      const uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
      out.push_back(alphabet[(v >> 18) & 0x3F]);
      out.push_back(alphabet[(v >> 12) & 0x3F]);
      out.push_back(alphabet[(v >> 6) & 0x3F]);
    }
    return out;
  }

  std::string x5t_s256(X509 *cert) {
    unsigned char *der = nullptr;
    const int der_len = i2d_X509(cert, &der);
    if (der_len <= 0 || der == nullptr) {
      return {};
    }
    const auto digest = crypto::hash(
      std::string_view(reinterpret_cast<const char *>(der), static_cast<std::size_t>(der_len)));
    OPENSSL_free(der);
    return base64url_encode(digest.data(), digest.size());
  }

}  // namespace nvhttp::token
