/**
 * @file src/nvhttp_token.h
 * @brief JWT (RS256) verifier for SwitchDesk Phase 2 token-based session start.
 *
 * The control plane mints short-lived RS256-signed JWTs; the engine verifies
 * them at each authenticated endpoint. Strict alg pinning closes the
 * alg-substitution footgun (alg=none, alg=HS256-with-pubkey-as-secret).
 *
 * See docs/phase-2-design.md in juvepr/switchdesk for the full design.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

namespace nvhttp::token {

  /**
   * @brief Parsed and validated JWT claims.
   *
   * Returned from verify() on success. `iss` and `aud` have already been
   * checked against `config::nvhttp.cp_issuer` and `config::nvhttp.node_id`
   * respectively; they are exposed for caller-side logging. `sub` and `sid`
   * are unenforced — `sid` is logged as audit context, `sub` is advisory.
   */
  struct claims_t {
    std::string iss;  ///< Issuer (matches config::nvhttp.cp_issuer)
    std::string sub;  ///< Subject — control-plane user id (advisory)
    std::string aud;  ///< Audience (matches config::nvhttp.node_id)
    std::string sid;  ///< Session id (advisory; logged, not enforced in v1)
    int64_t iat = 0;  ///< Issued-at unix timestamp (clock-skew-tolerated)
    int64_t exp = 0;  ///< Expiry unix timestamp
  };

  /**
   * @brief Load the control-plane issuer public key from a PEM-encoded
   *        X.509 certificate file.
   *
   * Called once at nvhttp::start() before any verify() call. On failure
   * (missing file, unreadable, malformed cert), logs a fatal-level message
   * and raises the shutdown event. Until a successful call lands, every
   * verify() returns std::nullopt (fail-closed).
   *
   * @param pem_path Filesystem path to the X.509 PEM file wrapping the
   *                 control plane's RSA-2048 public key.
   */
  void load_issuer_pubkey(const std::string &pem_path);

  /**
   * @brief Verify a JWT and return its validated claims.
   *
   * Production entry point. Uses `time(nullptr)` as "now"; delegates to
   * the two-arg overload.
   *
   * @param token The compact JWT (header.payload.signature, base64url).
   * @return Validated claims on success; std::nullopt on any failure
   *         (bad signature, expired, future-iat beyond clock skew, wrong
   *         audience, wrong issuer, malformed header/payload, alg not
   *         exactly "RS256", structural malformation, or pubkey not
   *         loaded). Failure mode is logged at debug level but is not
   *         distinguished to the caller.
   */
  std::optional<claims_t> verify(const std::string &token);

  /**
   * @brief Verify a JWT with an explicit "now" timestamp.
   *
   * Test-friendly overload. Lets unit tests use pre-baked fixtures with
   * absolute iat/exp values regardless of system clock — passing a fixed
   * "now" between fixture iat and exp values exercises the temporal
   * checks deterministically. Production callers use the one-arg
   * overload.
   *
   * @param token  The compact JWT.
   * @param now    Unix seconds to compare against iat/exp.
   * @return Same semantics as the one-arg overload.
   */
  std::optional<claims_t> verify(const std::string &token, int64_t now);

}  // namespace nvhttp::token
