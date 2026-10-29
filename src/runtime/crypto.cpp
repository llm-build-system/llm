module;

#include <openssl/evp.h>
#include <openssl/rand.h>

module llm.runtime;

import :internal;
import std;

namespace llm::runtime {
namespace {
std::string hexadecimal(std::span<const unsigned char> Bytes) {
  constexpr std::string_view Digits = "0123456789abcdef";
  std::string Text;
  Text.reserve(Bytes.size() * 2);
  for (auto Byte : Bytes) {
    Text.push_back(Digits[Byte >> 4]);
    Text.push_back(Digits[Byte & 15]);
  }
  return Text;
}

class Digest {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> State{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};

public:
  Digest() {
    if (!State || EVP_DigestInit_ex(State.get(), EVP_sha256(), nullptr) != 1)
      throw Failure("Cannot initialize SHA-256");
  }

  void update(std::span<const char> Bytes) {
    if (EVP_DigestUpdate(State.get(), Bytes.data(), Bytes.size()) != 1)
      throw Failure("SHA-256 update failed");
  }

  std::string finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> Bytes{};
    unsigned Length = 0;
    if (EVP_DigestFinal_ex(State.get(), Bytes.data(), &Length) != 1 ||
        Length != 32)
      throw Failure("SHA-256 finalization failed");
    return hexadecimal(std::span(Bytes).first(Length));
  }
};
}

std::string sha256Text(std::string_view Text) {
  Digest Hash;
  Hash.update(std::span(Text.data(), Text.size()));
  return Hash.finish();
}

std::string sha256File(const Path &File) {
  std::ifstream Input(nativePath(File), std::ios::binary);
  if (!Input)
    throw Failure("Cannot hash " + pathText(File));
  Digest Hash;
  std::array<char, 65536> Buffer;
  while (Input) {
    if (interrupted())
      throw Failure("Operation cancelled");
    Input.read(Buffer.data(), Buffer.size());
    Hash.update(
        std::span(Buffer).first(static_cast<std::size_t>(Input.gcount())));
  }
  if (!Input.eof())
    throw Failure("Cannot finish hashing " + pathText(File));
  return Hash.finish();
}

std::string randomHex(std::size_t Bytes) {
  if (Bytes > 1024 * 1024)
    throw Failure("Random identifier exceeds its size limit");
  std::vector<unsigned char> Buffer(Bytes);
  if (RAND_priv_bytes_ex(nullptr, Buffer.data(), Buffer.size(), 0) != 1)
    throw Failure("Cannot obtain cryptographically secure random bytes");
  return hexadecimal(Buffer);
}

}
