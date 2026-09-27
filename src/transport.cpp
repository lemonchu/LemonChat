#include "lemonchat/transport.hpp"
#include "lemonchat/crypto.hpp"
#include <openssl/ssl.h>
#include <cstring>

namespace lemon {
namespace {
void encode(unsigned char* output, std::uint64_t value, std::size_t size) {
    for (std::size_t i = size; i; --i) { output[i - 1] = static_cast<unsigned char>(value & 255); value >>= 8; }
}
std::uint64_t decode(const unsigned char* input, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value = (value << 8) | input[i];
    return value;
}
}
Packet json_packet(const Json& value) {
    const auto text = value.dump();
    if (text.size() + 1 > max_frame) throw std::runtime_error("Message is too large");
    auto packet = std::make_shared<std::vector<unsigned char>>(text.size() + 5);
    encode(packet->data(), text.size() + 1, 4);
    (*packet)[4] = 1;
    std::memcpy(packet->data() + 5, text.data(), text.size());
    return packet;
}
Packet chunk_packet(const std::string& id, std::uint64_t offset, const unsigned char* data, std::size_t size) {
    if (id.size() != 32 || size == 0 || size > chunk_size) throw std::runtime_error("Invalid binary chunk");
    auto packet = std::make_shared<std::vector<unsigned char>>(45 + size);
    encode(packet->data(), size + 41, 4);
    (*packet)[4] = 2;
    std::memcpy(packet->data() + 5, id.data(), 32);
    encode(packet->data() + 37, offset, 8);
    std::memcpy(packet->data() + 45, data, size);
    return packet;
}
Chunk parse_chunk(const Packet& packet) {
    if (packet->size() <= 45 || packet->size() > 45 + chunk_size || (*packet)[4] != 2)
        throw std::runtime_error("Invalid binary chunk size");
    std::string id(reinterpret_cast<const char*>(packet->data() + 5), 32);
    if (id.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid transfer ID");
    return {std::move(id), decode(packet->data() + 37, 8), packet->data() + 45, packet->size() - 45};
}
void configure_tls(asio::ssl::context& context) {
    if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_3_VERSION) != 1)
        throw std::runtime_error("TLS 1.3 is required");
    context.set_options(asio::ssl::context::no_compression);
    SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_max_early_data(context.native_handle(), 0);
}
Connection::Connection(asio::io_context& io, asio::ssl::context& tls) : stream_(io, tls), timer_(io) {}
Connection::Connection(Tcp::socket socket, asio::ssl::context& tls)
    : stream_(std::move(socket), tls), timer_(stream_.get_executor()) {}
void Connection::start(bool server, const std::string& pin) {
    asio::error_code ignored;
    stream_.next_layer().set_option(Tcp::no_delay(true), ignored);
    if (!server) {
        if (pin.size() != 64) throw std::runtime_error("A certificate fingerprint is required");
        stream_.set_verify_mode(asio::ssl::verify_peer);
        stream_.set_verify_callback([pin](bool, asio::ssl::verify_context& verification) {
            auto* context = verification.native_handle();
            if (X509_STORE_CTX_get_error_depth(context) != 0) return true;
            auto* certificate = X509_STORE_CTX_get_current_cert(context);
            try {
                return certificate && X509_cmp_current_time(X509_get0_notBefore(certificate)) < 0 &&
                    X509_cmp_current_time(X509_get0_notAfter(certificate)) > 0 && constant_equal(fingerprint(certificate), pin);
            } catch (...) { return false; }
        });
    }
    timer_.expires_after(std::chrono::seconds(10));
    timer_.async_wait([self = shared_from_this()](const asio::error_code& error) {
        if (!error) self->close("TLS handshake timed out");
    });
    stream_.async_handshake(server ? asio::ssl::stream_base::server : asio::ssl::stream_base::client,
        [self = shared_from_this()](const asio::error_code& error) {
            if (self->closed_) return;
            self->timer_.cancel();
            if (error) { self->close("TLS handshake failed (check the invitation fingerprint): " + error.message()); return; }
            self->established_ = true;
            self->last_read_ = std::chrono::steady_clock::now();
            self->watchdog();
            if (self->on_ready) self->on_ready();
            if (!self->closed_ && self->finish_reason_.empty()) self->read_header();
        });
}
void Connection::watchdog() {
    timer_.expires_after(std::chrono::seconds(5));
    timer_.async_wait([self = shared_from_this()](const asio::error_code& error) {
        if (error || self->closed_) return;
        if (std::chrono::steady_clock::now() - self->last_read_ > std::chrono::seconds(45)) {
            self->close("Peer stopped responding"); return;
        }
        self->watchdog();
    });
}
void Connection::read_header() {
    asio::async_read(stream_, asio::buffer(header_), [self = shared_from_this()](const asio::error_code& error, std::size_t) {
        if (self->closed_) return;
        if (error) { self->close("Connection closed: " + error.message()); return; }
        const auto length = decode(self->header_.data(), 4);
        if (length < 2 || length > max_frame) { self->close("Invalid frame length"); return; }
        self->read_body(static_cast<std::size_t>(length));
    });
}
void Connection::read_body(std::size_t length) {
    auto packet = std::make_shared<std::vector<unsigned char>>(length + 4);
    std::copy(header_.begin(), header_.end(), packet->begin());
    asio::async_read(stream_, asio::buffer(packet->data() + 4, length),
        [self = shared_from_this(), packet](const asio::error_code& error, std::size_t) {
            if (self->closed_) return;
            if (error) { self->close("Incomplete frame: " + error.message()); return; }
            self->last_read_ = std::chrono::steady_clock::now();
            try {
                if ((*packet)[4] == 1) {
                    bool excessive_depth = false;
                    const auto value = Json::parse(packet->begin() + 5, packet->end(),
                        [&excessive_depth](int depth, Json::parse_event_t, Json&) {
                            if (depth > 16) { excessive_depth = true; return false; }
                            return true;
                        });
                    if (excessive_depth || !value.is_object()) throw std::runtime_error("Invalid control message");
                    if (self->on_json) self->on_json(value);
                } else if ((*packet)[4] == 2) {
                    (void)parse_chunk(packet);
                    if (self->on_chunk) self->on_chunk(packet);
                } else throw std::runtime_error("Unknown frame type");
            } catch (const std::exception& exception) {
                self->close(std::string("Protocol error: ") + exception.what());
                return;
            }
            if (!self->closed_ && self->finish_reason_.empty()) self->read_header();
        });
}
bool Connection::send(const Json& value) { return send(json_packet(value)); }
bool Connection::send(Packet packet) {
    if (closed_ || !established_ || !finish_reason_.empty()) return false;
    if (packet->size() > 8 * 1024 * 1024 - queued_) { close("Outgoing queue full; peer is too slow"); return false; }
    queued_ += packet->size();
    output_.push_back(std::move(packet));
    if (output_.size() == 1) write();
    return true;
}
void Connection::write() {
    if (closed_) return;
    if (output_.empty()) {
        if (!finish_reason_.empty()) close(finish_reason_);
        return;
    }
    const auto packet = output_.front();
    asio::async_write(stream_, asio::buffer(*packet),
        [self = shared_from_this(), packet](const asio::error_code& error, std::size_t) {
            if (self->closed_) return;
            if (error) { self->close("Send failed: " + error.message()); return; }
            self->queued_ -= packet->size();
            self->output_.pop_front();
            self->write();
        });
}
void Connection::finish(const std::string& reason) {
    finish_reason_ = reason;
    if (output_.empty()) close(reason);
}
void Connection::close(const std::string& reason) {
    if (closed_) return;
    closed_ = true;
    timer_.cancel();
    asio::error_code ignored;
    stream_.next_layer().cancel(ignored);
    stream_.next_layer().shutdown(Tcp::socket::shutdown_both, ignored);
    stream_.next_layer().close(ignored);
    output_.clear();
    queued_ = 0;
    if (on_close) on_close(reason);
}
} // namespace lemon
