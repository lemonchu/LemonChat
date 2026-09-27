#pragma once
#include "server.hpp"
#include <filesystem>
#include <thread>

namespace lemon {
struct ClientOptions {
    std::string name = "lemon";
    Invitation invitation;
    std::filesystem::path downloads;
    std::uint64_t max_file = default_max_file;
};
class Client {
public:
    Client(asio::io_context& io, ClientOptions options, EventSink sink);
    ~Client();
    void connect();
    void command(Json value);
    void close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct AppOptions {
    bool host = false;
    ServerOptions server;
    ClientOptions client;
    std::filesystem::path data_directory;
};
// The facade is called by the UI/CLI thread; all core operations are posted to its joined event loop.
class Application {
public:
    Application(AppOptions options, EventSink events);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    void command(Json value);
    void stop();
    const std::string& invitation() const { return invitation_; }
private:
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io_)};
    std::unique_ptr<Server> server_;
    std::unique_ptr<Client> client_;
    EventSink events_;
    std::thread worker_;
    std::string invitation_;
};
} // namespace lemon
