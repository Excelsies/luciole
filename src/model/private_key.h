#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace luciole::private_key_detail {
  using namespace std::string_view_literals;

  inline constexpr std::array kMarkers{
      "-----BEGIN PGP PRIVATE KEY BLOCK-----"sv,
      "-----BEGIN PGP SECRET KEY BLOCK-----"sv,
      "---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----"sv,
      "PuTTY-User-Key-File-"sv,
      "AGE-SECRET-KEY-1"sv,
      "AGE-SECRET-KEY-PQ-1"sv
  };
  inline constexpr std::array kKeyContentTypes{
      "application/pgp-keys"sv,        "application/pkcs10"sv,
      "application/pkcs10+pem"sv,      "application/pkcs12"sv,
      "application/pkcs12+pem"sv,      "application/pkcs7-mime"sv,
      "application/pkcs7-mime+pem"sv,  "application/pkcs7-signature"sv,
      "application/pkcs8"sv,           "application/pkcs8+pem"sv,
      "application/pkcs8-encrypted"sv, "application/pkix-cert"sv,
      "application/pkix-cert+pem"sv,   "application/pkix-crl"sv,
      "application/pkix-crl+pem"sv,    "application/pkix-pkipath"sv,
      "application/x-pem-file"sv,      "application/x-pem-key"sv,
      "application/x-pkcs12"sv,        "application/x-pkcs7-certificates"sv,
      "application/x-x509-ca-cert"sv,  "application/x-x509-user-cert"sv,
  };
  inline constexpr auto kPemBegin = "-----BEGIN "sv;
  // "-----BEGIN" in base64, which starts every base64-encoded PEM file.
  inline constexpr auto kBase64PemBegin = "LS0tLS1CRUdJTi"sv;
  // Digits decoded at each kBase64PemBegin: 96 bytes, more than any PEM BEGIN line.
  inline constexpr std::size_t kBase64Digits = 128;
  inline constexpr std::array kWireGuardKeys{"privatekey"sv, "presharedkey"sv};

  inline int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
      return c - '0' + 52;
    }
    if (c == '+') {
      return 62;
    }
    return c == '/' ? 63 : -1;
  }

  // Decodes standard base64 digits up to the first other character.
  inline std::string decodeBase64(std::string_view digits) {
    std::string bytes;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : digits) {
      const int value = base64Value(c);
      if (value < 0) {
        break;
      }
      bits = ((bits << 6U) | static_cast<std::uint32_t>(value)) & 0xFFFFU;
      count += 6;
      if (count >= 8) {
        count -= 8;
        bytes += static_cast<char>((bits >> static_cast<unsigned>(count)) & 0xFFU);
      }
    }
    return bytes;
  }

  inline std::string_view skipBlanks(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
  }

  // "-----BEGIN " with "PRIVATE KEY-----" later on the same line.
  inline bool pemPrivateKeyLine(std::string_view line) {
    const auto opening = line.find(kPemBegin);
    const auto closing = line.rfind("PRIVATE KEY-----");
    return opening != std::string_view::npos
        && closing != std::string_view::npos
        && closing >= opening + kPemBegin.size();
  }

  // Whether text starts with the lowercase prefix in any letter case.
  inline bool startsWithLowercase(std::string_view text, std::string_view prefix) {
    return std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) == b;
    });
  }

  // A WireGuard PrivateKey or PresharedKey entry, any case: 43 base64 digits and "=", then nothing or a comment.
  inline bool wireGuardKeyLine(std::string_view line) {
    if (line.starts_with("\xEF\xBB\xBF")) {
      line.remove_prefix(3);
    }
    line = skipBlanks(line);
    std::size_t nameSize = 0;
    for (const auto key : kWireGuardKeys) {
      if (startsWithLowercase(line, key)) {
        nameSize = key.size();
      }
    }
    if (nameSize == 0) {
      return false;
    }
    line = skipBlanks(line.substr(nameSize));
    if (!line.starts_with('=')) {
      return false;
    }
    const auto value = skipBlanks(line.substr(1));
    if (value.size() < 44 || value[43] != '=' || !std::ranges::all_of(value.substr(0, 43), [](char c) {
          return base64Value(c) >= 0;
        })) {
      return false;
    }
    const auto rest = skipBlanks(value.substr(44));
    return rest.empty() || rest.starts_with('#');
  }

  // The markers and line rules over plain text, with lines split at CR and LF.
  inline bool plainPrivateKey(std::string_view bytes) {
    if (std::ranges::any_of(kMarkers, [&](std::string_view marker) { return bytes.contains(marker); })) {
      return true;
    }
    for (std::size_t start = 0; start < bytes.size();) {
      const auto end = std::min(bytes.find_first_of("\r\n", start), bytes.size());
      const auto line = bytes.substr(start, end - start);
      if (pemPrivateKeyLine(line) || wireGuardKeyLine(line)) {
        return true;
      }
      start = end + 1;
    }
    return false;
  }
} // namespace luciole::private_key_detail

namespace luciole {
  // Content types of certificate and key containers, PEM text or binary. A MIME database types a .pem in several of
  // these ways, so a preview does not go by the type: it reads these files and goes by the bytes. PEM text is shown
  // unless it holds a private key; a binary container (DER, PKCS#12, a keyring) is never shown, as text or as hex.
  inline bool isKeyContentType(std::string_view type) {
    return std::ranges::find(private_key_detail::kKeyContentTypes, type) != private_key_detail::kKeyContentTypes.end();
  }

  // Whether the bytes are text rather than a binary container: valid UTF-8 (a BOM is valid) with no control byte
  // below 0x20 other than tab, line breaks and form feed. A character cut at the end of the bytes read is fine when
  // `cut` says the file goes on. DER, PKCS#12 and keyrings have NUL or other control bytes, or invalid UTF-8.
  inline bool looksLikePemText(std::string_view bytes, bool cut = false) {
    for (std::size_t i = 0; i < bytes.size();) {
      const auto byte = static_cast<unsigned char>(bytes[i]);
      if (byte < 0x80) {
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f') {
          return false;
        }
        ++i;
        continue;
      }
      const std::size_t length = byte >= 0xC2 && byte <= 0xDF
          ? 2
          : (byte >= 0xE0 && byte <= 0xEF ? 3 : (byte >= 0xF0 && byte <= 0xF4 ? 4 : 0));
      if (length == 0) {
        return false;
      }
      for (std::size_t k = 1; k < length; ++k) {
        if (i + k >= bytes.size()) {
          return cut; // the sequence runs past the bytes read
        }
        if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0U) != 0x80U) {
          return false;
        }
      }
      i += length;
    }
    return true;
  }

  // Whether the bytes hold private key material anywhere: a PEM BEGIN line naming a private key, a PGP, SSH2, PuTTY or
  // age key marker, a WireGuard PrivateKey or PresharedKey entry, or a base64-encoded PEM private key. Makes a fixed
  // number of scans over the given bytes plus one decode of at most 128 digits at each base64 PEM start.
  inline bool containsPrivateKey(std::string_view bytes) {
    using namespace private_key_detail;
    if (plainPrivateKey(bytes)) {
      return true;
    }
    for (auto at = bytes.find(kBase64PemBegin); at != std::string_view::npos;
         at = bytes.find(kBase64PemBegin, at + 1)) {
      if (plainPrivateKey(decodeBase64(bytes.substr(at, kBase64Digits)))) {
        return true;
      }
    }
    return false;
  }
} // namespace luciole
