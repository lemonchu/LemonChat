#include "lemonchat/crypto.hpp"
#include "lemonchat/common.hpp"
#include "lemonchat/storage.hpp"
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <cctype>
#include <fstream>
#include <regex>
#include <vector>

namespace lemon {
namespace {
std::string hex(const unsigned char* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const auto size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}
Identity generate_identity() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    require(ctx && EVP_PKEY_keygen_init(ctx.get()) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) == 1, "Initialize TLS key generation");
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_keygen(ctx.get(), &raw_key) == 1, "Generate TLS key");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    require(static_cast<bool>(cert), "Allocate TLS certificate");
    require(X509_set_version(cert.get(), 2) == 1, "Set certificate version");
    unsigned char serial[8];
    require(RAND_bytes(serial, sizeof(serial)) == 1, "Generate certificate serial");
    std::uint64_t serial_number = 1;
    for (unsigned char byte : serial) serial_number = (serial_number << 8) | byte;
    require(ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()), serial_number) == 1, "Set certificate serial");
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) &&
            X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3L * 365 * 86400), "Set certificate dates");
    require(X509_set_pubkey(cert.get(), key.get()) == 1, "Set certificate key");
    auto* name = X509_get_subject_name(cert.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("LemonChat"), -1, -1, 0) == 1, "Set certificate name");
    require(X509_set_issuer_name(cert.get(), name) == 1 && X509_sign(cert.get(), key.get(), EVP_sha256()) > 0,
            "Sign TLS certificate");
    std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(BIO_new(BIO_s_mem()), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(BIO_new(BIO_s_mem()), BIO_free);
    require(cert_bio && key_bio && PEM_write_bio_X509(cert_bio.get(), cert.get()) == 1 &&
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
        "Encode TLS identity");
    return {bio_string(cert_bio.get()), bio_string(key_bio.get()), fingerprint(cert.get()), random_hex(32)};
}
} // namespace

std::string random_hex(std::size_t bytes) {
    if (bytes > 1024) throw std::runtime_error("Random request too large");
    std::vector<unsigned char> buffer(bytes);
    require(RAND_bytes(buffer.data(), static_cast<int>(bytes)) == 1, "Secure random generation failed");
    return hex(buffer.data(), buffer.size());
}
std::string fingerprint(X509* certificate) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    require(certificate && X509_digest(certificate, EVP_sha256(), digest, &length) == 1, "Read certificate fingerprint");
    return hex(digest, length);
}
bool constant_equal(const std::string& left, const std::string& right) {
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
Sha256::Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    require(context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1, "Initialize SHA-256");
}
void Sha256::update(const void* data, std::size_t size) {
    require(!finished_ && EVP_DigestUpdate(context_.get(), data, size) == 1, "Update SHA-256");
}
std::string Sha256::finish() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    require(!finished_ && EVP_DigestFinal_ex(context_.get(), digest, &size) == 1, "Finish SHA-256");
    finished_ = true;
    return hex(digest, size);
}
Identity load_identity(const std::filesystem::path& directory) {
    private_directory(directory);
    const auto path = directory / "identity.json";
    if (!std::filesystem::exists(std::filesystem::symlink_status(path))) {
        auto identity = generate_identity();
        write_new_file(path, Json{{"certificate", identity.certificate}, {"private_key", identity.private_key},
                                 {"secret", identity.secret}}.dump(2));
        return identity;
    }
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) || std::filesystem::file_size(path) > 32768)
        throw std::runtime_error("Invalid identity file");
    std::ifstream stream(path, std::ios::binary);
    const auto value = Json::parse(stream);
    Identity identity{string_field(value, "certificate", 16384), string_field(value, "private_key", 16384), "",
                      string_field(value, "secret", 64)};
    require(std::regex_match(identity.secret, std::regex("[0-9a-f]{64}")), "Invalid invitation secret");
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(identity.certificate.data(), static_cast<int>(identity.certificate.size())), BIO_free);
    require(static_cast<bool>(bio), "Read identity");
    std::unique_ptr<X509, decltype(&X509_free)> certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    require(static_cast<bool>(certificate), "Invalid certificate in identity");
    identity.pin = fingerprint(certificate.get());
    return identity;
}
std::string Invitation::str() const {
    return "lemonchat://" + (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
        std::to_string(port) + "?pin=" + pin + "&key=" + secret;
}
Invitation Invitation::parse(const std::string& text) {
    if (text.size() > 1024) throw std::runtime_error("Invitation is too long");
    static const std::regex pattern(R"(^lemonchat://(\[[^\]\s]+\]|[^:/?#\s]+):([0-9]{1,5})\?pin=([0-9a-fA-F]{64})&key=([0-9a-fA-F]{64})$)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) throw std::runtime_error("Invalid invitation; paste the complete lemonchat:// link");
    const auto port = std::stoul(match[2]);
    if (port == 0 || port > 65535) throw std::runtime_error("Invalid invitation port");
    Invitation result{match[1], static_cast<std::uint16_t>(port), match[3], match[4]};
    if (result.host.front() == '[') result.host = result.host.substr(1, result.host.size() - 2);
    std::transform(result.pin.begin(), result.pin.end(), result.pin.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(result.secret.begin(), result.secret.end(), result.secret.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}
} // namespace lemon
