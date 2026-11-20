#include "model/private_key.h"
#include "test_check.h"

#include <format>
#include <print>
#include <string>
#include <string_view>

namespace {
  // Made-up key bodies: the shape of each format, never real key material.
  std::string pem(std::string_view label, std::string_view newline = "\n", std::string_view indent = "") {
    return std::format(
        "{0}-----BEGIN {1}-----{2}{0}MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCfakebody{2}"
        "{0}Zm9yIHRlc3RzIG9ubHkgbm90IGEgcmVhbCBrZXkgYm9keSBhdCBhbGw={2}{0}-----END {1}-----{2}",
        indent, label, newline
    );
  }

  // 43 base64 digits and "=", the shape of a 32-byte WireGuard key.
  constexpr std::string_view kWireGuardValue = "yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAAA=";

  void formatChecks() {
    for (const auto* label :
         {"RSA PRIVATE KEY", "EC PRIVATE KEY", "DSA PRIVATE KEY", "PRIVATE KEY", "ENCRYPTED PRIVATE KEY",
          "OPENSSH PRIVATE KEY"}) {
      TEST_CHECK(luciole::containsPrivateKey(pem(label)));
    }
    TEST_CHECK(
        luciole::containsPrivateKey(
            "-----BEGIN PGP PRIVATE KEY BLOCK-----\n\nlQOYBGfakebodyAQgAfakebody\n=abcd\n"
            "-----END PGP PRIVATE KEY BLOCK-----\n"
        )
    );
    TEST_CHECK(
        luciole::containsPrivateKey(
            "PuTTY-User-Key-File-3: ssh-ed25519\nEncryption: none\nComment: throwaway\nPublic-Lines: 2\n"
            "AAAAC3NzaC1lZDI1NTE5AAAAIfakebody\nfakebody\nPrivate-Lines: 1\nAAAAIfakebody\nPrivate-MAC: 00ff\n"
        )
    );
    TEST_CHECK(luciole::containsPrivateKey("PuTTY-User-Key-File-2: ssh-rsa\nEncryption: none\n"));
    TEST_CHECK(
        luciole::containsPrivateKey(
            "# created: 2026-01-01T00:00:00Z\n# public key: age1fakepublickey\n"
            "AGE-SECRET-KEY-1QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ\n"
        )
    );
    TEST_CHECK(
        luciole::containsPrivateKey(
            "---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----\nComment: \"throwaway\"\nP2/56wAAAfakebody\n"
            "---- END SSH2 ENCRYPTED PRIVATE KEY ----\n"
        )
    );
    TEST_CHECK(
        luciole::containsPrivateKey(
            "-----BEGIN PGP SECRET KEY BLOCK-----\nVersion: 2.6.3i\n\nlQOYBGfakebody\n=abcd\n"
            "-----END PGP SECRET KEY BLOCK-----\n"
        )
    );
    TEST_CHECK(luciole::containsPrivateKey("AGE-SECRET-KEY-PQ-1QQQQQQQQQQQQQQQQQQQQQQQQ\n"));
  }

  void base64Checks() {
    // base64 -w0 of an EC key and of a PKCS#8 key with made-up bodies.
    TEST_CHECK(
        luciole::containsPrivateKey(
            "users:\n- name: default\n  user:\n    client-key-data: "
            "LS0tLS1CRUdJTiBFQyBQUklWQVRFIEtFWS0tLS0tCk1IY0NBUUVFSWZha2Vib2R5ZmFrZWJvZHlmYWtlYm9keWZha2Vib2R5Ci0tLS0tRU"
            "5E"
            "IEVDIFBSSVZBVEUgS0VZLS0tLS0K\n"
        )
    );
    TEST_CHECK(
        luciole::containsPrivateKey(
            "apiVersion: v1\nkind: Secret\ntype: kubernetes.io/tls\ndata:\n  tls.key: "
            "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1JSUV2UUlCQURBTmZha2Vib2R5ZmFrZWJvZHlmYWtlYm9keQotLS0tLUVORCBQUklWQV"
            "RF"
            "IEtFWS0tLS0tCg==\n"
        )
    );
    // base64 wrapped at 76 columns: the first line holds the whole BEGIN line.
    TEST_CHECK(
        luciole::containsPrivateKey(
            "LS0tLS1CRUdJTiBFTkNSWVBURUQgUFJJVkFURSBLRVktLS0tLQpNSUlGSERCT0Jna3Foa2lHOXcw\r\n"
            "QkJRMHdRVEFwQmdrcWhraUc5dzBCQlF3d0hBUUlmYWtlYm9keWZha2Vib2R5Ci0tLS0tRU5EIEVO\r\n"
        )
    );
    TEST_CHECK(!luciole::containsPrivateKey(
        "    client-certificate-data: "
        "LS0tLS1CRUdJTiBDRVJUSUZJQ0FURS0tLS0tCk1JSUJmYWtlY2VydGlmaWNhdGVib2R5ZmFrZWNlcnRpZmljYXRlYm9keQotLS0tLUVORCBD"
        "RVJUSUZJQ0FURS0tLS0tCg==\n"
    ));
    TEST_CHECK(!luciole::containsPrivateKey(
        "  tls.pub: LS0tLS1CRUdJTiBQVUJMSUMgS0VZLS0tLS0KTUNvd0JRWURLMlZ3QXlFQWZha2VwdWJsaWNrZXlib2R5Ci0tLS0tRU5EIFBV"
        "QkxJQyBLRVktLS0tLQo=\n"
    ));
    TEST_CHECK(!luciole::containsPrivateKey("value: LS0tLS1CRUdJTi!!!! not base64 at all\n"));
    TEST_CHECK(!luciole::containsPrivateKey("value: LS0tLS1CRUdJTiBQUklW"));
    TEST_CHECK(!luciole::containsPrivateKey("value: LS0tLS1CRUdJTi"));
  }

  void wireGuardChecks() {
    TEST_CHECK(kWireGuardValue.size() == 44);
    const auto config = [](std::string_view line) {
      return std::format("[Interface]\nAddress = 10.0.0.2/32\n{}\nListenPort = 51820\n", line);
    };
    TEST_CHECK(luciole::containsPrivateKey(config(std::format("PrivateKey = {}", kWireGuardValue))));
    TEST_CHECK(luciole::containsPrivateKey(config(std::format("PrivateKey={}", kWireGuardValue))));
    TEST_CHECK(luciole::containsPrivateKey(config(std::format("PresharedKey = {}", kWireGuardValue))));
    TEST_CHECK(luciole::containsPrivateKey(config(std::format("  \tPrivateKey\t=  {}  ", kWireGuardValue))));
    TEST_CHECK(luciole::containsPrivateKey(config(std::format("privatekey = {} # laptop", kWireGuardValue))));
    TEST_CHECK(luciole::containsPrivateKey(std::format("[Interface]\r\nPrivateKey = {}\r\n", kWireGuardValue)));
    TEST_CHECK(luciole::containsPrivateKey(std::format("\xEF\xBB\xBFPrivateKey = {}", kWireGuardValue)));
    TEST_CHECK(
        luciole::containsPrivateKey(
            std::format("[NetDev]\nKind=wireguard\n[WireGuard]\nPrivateKey={}\n", kWireGuardValue)
        )
    );
    TEST_CHECK(!luciole::containsPrivateKey(config(std::format("PublicKey = {}", kWireGuardValue))));
    TEST_CHECK(!luciole::containsPrivateKey(config(std::format("PrivateKeyFile = {}", kWireGuardValue))));
    TEST_CHECK(!luciole::containsPrivateKey(config(std::format("MyPrivateKey = {}", kWireGuardValue))));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKeyFile = /etc/wireguard/private.key")));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKey = yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAA=")));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKey = yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAAAA=")));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKey = yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAAAA")));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKey = yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAAA= extra")));
    TEST_CHECK(!luciole::containsPrivateKey(config("PrivateKey = yAnzfakeWireGuard*eyForTestsOnlyAAAAAAAAAAA=")));
  }

  void placementChecks() {
    TEST_CHECK(luciole::containsPrivateKey("# deploy key for the staging host\n\n" + pem("OPENSSH PRIVATE KEY")));
    TEST_CHECK(luciole::containsPrivateKey(pem("CERTIFICATE") + pem("PRIVATE KEY")));
    TEST_CHECK(luciole::containsPrivateKey(pem("CERTIFICATE") + pem("CERTIFICATE") + pem("RSA PRIVATE KEY")));
    TEST_CHECK(
        luciole::containsPrivateKey(
            "{\"type\": \"service_account\", \"private_key\": \"-----BEGIN PRIVATE KEY-----\\nMIIEfakebody\\n"
            "-----END PRIVATE KEY-----\\n\"}\n"
        )
    );
    TEST_CHECK(luciole::containsPrivateKey("SOPS_AGE_KEY=AGE-SECRET-KEY-1QQQQQQQQQQQQQQQQ\n"));
    // Only the start of the key was read.
    TEST_CHECK(luciole::containsPrivateKey(std::string(60U * 1024U, 'x') + "\n-----BEGIN RSA PRIVATE KEY-----\nMII"));
  }

  void evasionChecks() {
    TEST_CHECK(luciole::containsPrivateKey("\xEF\xBB\xBF" + pem("RSA PRIVATE KEY")));
    TEST_CHECK(luciole::containsPrivateKey(pem("EC PRIVATE KEY", "\r\n")));
    TEST_CHECK(luciole::containsPrivateKey(pem("PRIVATE KEY", "\r")));
    TEST_CHECK(luciole::containsPrivateKey(pem("OPENSSH PRIVATE KEY", "\n", "    ")));
    TEST_CHECK(luciole::containsPrivateKey(pem("OPENSSH PRIVATE KEY", "\n", "\t")));
    TEST_CHECK(luciole::containsPrivateKey("\xEF\xBB\xBF  \t" + pem("ENCRYPTED PRIVATE KEY", "\r\n")));
    TEST_CHECK(luciole::containsPrivateKey("tls:\n  key: |\n" + pem("RSA PRIVATE KEY", "\n", "    ")));
    TEST_CHECK(
        luciole::containsPrivateKey(
            "\xEF\xBB\xBF"
            "PuTTY-User-Key-File-3: ssh-ed25519\r\n"
        )
    );
    TEST_CHECK(luciole::containsPrivateKey("  \tAGE-SECRET-KEY-1QQQQQQQQQQQQQQQQ\r\n"));
    TEST_CHECK(luciole::containsPrivateKey("\r\n  -----BEGIN PGP PRIVATE KEY BLOCK-----\r\n"));
  }

  void previewableChecks() {
    TEST_CHECK(!luciole::containsPrivateKey(""));
    TEST_CHECK(!luciole::containsPrivateKey(pem("CERTIFICATE")));
    TEST_CHECK(!luciole::containsPrivateKey(pem("PUBLIC KEY")));
    TEST_CHECK(!luciole::containsPrivateKey(pem("RSA PUBLIC KEY")));
    TEST_CHECK(!luciole::containsPrivateKey(pem("CERTIFICATE REQUEST")));
    TEST_CHECK(!luciole::containsPrivateKey(pem("PGP PUBLIC KEY BLOCK")));
    TEST_CHECK(!luciole::containsPrivateKey(
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBfakepublickeybody throwaway@example\n"
        "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQfakepublickeybody throwaway@example\n"
    ));
    TEST_CHECK(!luciole::containsPrivateKey("Keep your private key secret. Never share a PRIVATE KEY with anyone.\n"));
    TEST_CHECK(!luciole::containsPrivateKey("-----BEGIN \nRSA PRIVATE KEY-----\n"));
    TEST_CHECK(!luciole::containsPrivateKey("-----BEGIN \rRSA PRIVATE KEY-----\n"));
    TEST_CHECK(!luciole::containsPrivateKey("-----END PRIVATE KEY----- -----BEGIN CERTIFICATE-----\n"));
    TEST_CHECK(!luciole::containsPrivateKey("# public key: age1fakepublickey\n"));
  }
} // namespace

int main() {
  formatChecks();
  base64Checks();
  wireGuardChecks();
  placementChecks();
  evasionChecks();
  previewableChecks();
  std::println("private key checks passed");
  return 0;
}
