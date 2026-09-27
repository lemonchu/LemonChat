#pragma once
#include "common.hpp"
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace lemon {
using Tcp = asio::ip::tcp;
using Packet = std::shared_ptr<const std::vector<unsigned char>>;
Packet json_packet(const Json& value);
Packet chunk_packet(const std::string& id, std::uint64_t offset, const unsigned char* data, std::size_t size);
struct Chunk {
    std::string id;
    std::uint64_t offset;
    const unsigned char* data;
    std::size_t size;
};
Chunk parse_chunk(const Packet& packet);
void configure_tls(asio::ssl::context& context);

// All methods and callbacks run on one owning io_context thread.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(asio::io_context& io, asio::ssl::context& tls);
    Connection(Tcp::socket socket, asio::ssl::context& tls);
    Tcp::socket& socket() { return stream_.next_layer(); }
    void start(bool server, const std::string& pin = "");
    bool send(const Json& value);
    bool send(Packet packet);
    void close(const std::string& reason);
    void finish(const std::string& reason);
    bool closed() const { return closed_; }
    std::function<void()> on_ready;
    std::function<void(const Json&)> on_json;
    std::function<void(Packet)> on_chunk;
    std::function<void(const std::string&)> on_close;
private:
    void read_header();
    void read_body(std::size_t length);
    void write();
    void watchdog();
    asio::ssl::stream<Tcp::socket> stream_;
    asio::steady_timer timer_;
    std::array<unsigned char, 4> header_{};
    std::deque<Packet> output_;
    std::size_t queued_ = 0;
    bool closed_ = false;
    bool established_ = false;
    std::string finish_reason_;
    std::chrono::steady_clock::time_point last_read_ = std::chrono::steady_clock::now();
};
} // namespace lemon
