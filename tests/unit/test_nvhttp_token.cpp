/**
 * @file tests/unit/test_nvhttp_token.cpp
 * @brief Test src/nvhttp_token.* — JWT verifier for SwitchDesk Phase 2.
 *
 * Covers the 10 cases from docs/phase-2-design.md: valid passes, expired
 * rejected, future-iat rejected, wrong-aud rejected, wrong-iss rejected,
 * bad-signature rejected, alg=none rejected, alg=HS256 rejected,
 * malformed-not-3-parts rejected, malformed-JSON rejected.
 *
 * Fixtures pre-baked by tools/bake-jwt-fixtures.py in the monorepo,
 * against the dev RSA keypair. Test passes a fixed "now" timestamp into
 * verify() so fixture iat/exp values stay stable regardless of when
 * tests run (good through 2033, well past v1).
 */
// test imports
#include "../tests_common.h"

// lib imports
#include <fstream>
#include <sstream>
#include <string>

// local imports
#include <src/config.h>
#include <src/nvhttp_token.h>

namespace {
  // Fixed "now". Sits between valid.iat (1700000000) and valid.exp
  // (2000000000), after expired.exp (1500000300), before future-iat.iat
  // (3000000000).
  constexpr int64_t kFixedNow = 1800000000;  // 2027-01-15 00:00:00 UTC

  constexpr const char *kExpectedAud = "sd-kc-01";
  constexpr const char *kExpectedIss = "switchdesk-cp-dev";

  std::string load_fixture(const std::string &name) {
    const std::string path = std::string(SUNSHINE_SOURCE_DIR) +
                             "/tests/unit/fixtures/" + name;
    std::ifstream in(path);
    if (!in) {
      ADD_FAILURE() << "Failed to open fixture: " << path;
      return "";
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
      content.pop_back();
    }
    return content;
  }
}  // namespace

struct NvhttpTokenTest: testing::Test {
  static void SetUpTestSuite() {
    const std::string pubkey_path = std::string(SUNSHINE_SOURCE_DIR) +
                                    "/tests/unit/fixtures/cp-pubkey.pem";
    nvhttp::token::load_issuer_pubkey(pubkey_path);
    config::nvhttp.cp_issuer = kExpectedIss;
    config::nvhttp.node_id = kExpectedAud;
  }
};

TEST_F(NvhttpTokenTest, ValidTokenPasses) {
  const auto claims = nvhttp::token::verify(load_fixture("jwt-valid.txt"), kFixedNow);
  ASSERT_TRUE(claims.has_value());
  EXPECT_EQ(claims->iss, kExpectedIss);
  EXPECT_EQ(claims->aud, kExpectedAud);
  EXPECT_EQ(claims->sub, "test-session");
  EXPECT_EQ(claims->sid, "test-session");
  EXPECT_EQ(claims->iat, 1700000000);
  EXPECT_EQ(claims->exp, 2000000000);
}

TEST_F(NvhttpTokenTest, ExpiredRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-expired.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, FutureIatRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-future-iat.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, WrongAudienceRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-wrong-aud.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, WrongIssuerRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-wrong-iss.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, BadSignatureRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-bad-sig.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, AlgNoneRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-alg-none.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, AlgHS256Rejected) {
  // alg-substitution attack: token claims HS256 with our pubkey PEM as the
  // HMAC secret. Strict alg-pinning must reject this BEFORE signature
  // verification, otherwise a verifier that defers alg checking would
  // accept the forgery.
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-alg-hs256.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, MalformedNotThreePartsRejected) {
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-malformed-not-3-parts.txt"), kFixedNow).has_value());
}

TEST_F(NvhttpTokenTest, MalformedJsonRejected) {
  // Signature verifies (signed over the malformed bytes), but JSON parse
  // of the payload section fails. Exercises the post-signature
  // structural-validation path.
  EXPECT_FALSE(nvhttp::token::verify(load_fixture("jwt-malformed-json.txt"), kFixedNow).has_value());
}
