#pragma once
#include "crypto.hpp"
#include "transport.hpp"

namespace lemon {
struct ServerOptions {
    std::string bind = "0.0.0.0";
    std::string advertise = "127.0.0.1";
    std::uint16_t port = 4567;
    std::uint64_t max_file = default_max_file;
    std::size_t max_clients = 64;
};
class Server {
public:
    Server(asio::io_context& io, Identity identity, ServerOptions options, EventSink events = {});
    ~Server();
    void start();
    void stop();
    Invitation invitation() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace lemon
