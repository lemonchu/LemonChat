#pragma once
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lemon {
std::string random_hex(std::size_t bytes = 16);
std::string fingerprint(X509* certificate);
bool constant_equal(const std::string& left, const std::string& right);

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::string finish();
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_{nullptr, EVP_MD_CTX_free};
    bool finished_ = false;
};

struct Identity {
    std::string certificate;
    std::string private_key;
    std::string pin;
    std::string secret;
};
Identity load_identity(const std::filesystem::path& directory);

struct Invitation {
    std::string host;
    std::uint16_t port = 0;
    std::string pin;
    std::string secret;
    std::string str() const;
    static Invitation parse(const std::string& text);
};
} // namespace lemon
