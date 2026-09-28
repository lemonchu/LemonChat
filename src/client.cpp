#include "lemonchat/client.hpp"
#include "lemonchat/storage.hpp"
#include <fstream>
#include <map>

namespace lemon {
namespace fs = std::filesystem;
struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
    struct Source {
        fs::path path;
        std::uint64_t size = 0;
    };
    struct Outgoing {
        Source source;
        std::string id, offer;
        std::ifstream input;
        Sha256 hash;
        std::uint64_t sent = 0, acknowledged = 0;
        bool ended = false;
        std::chrono::steady_clock::time_point progress{};
    };
    struct Incoming {
        std::string id, offer, filename;
        std::uint64_t size = 0, received = 0;
        std::unique_ptr<IncomingFile> file;
        Sha256 hash;
        bool saved = false;
        std::chrono::steady_clock::time_point progress{};
    };
    asio::io_context& io;
    ClientOptions options;
    EventSink events;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    Tcp::resolver resolver;
    asio::steady_timer connect_timer;
    std::shared_ptr<Connection> connection;
    std::string self_id;
    bool ready = false;
    unsigned generation = 0;
    std::uint64_t max_send_file = default_max_file;
    std::map<std::string, Source> sources;
    std::map<std::string, Json> offers;
    std::map<std::string, std::shared_ptr<Incoming>> accepted;
    std::map<std::string, std::shared_ptr<Incoming>> incoming;
    std::map<std::string, std::shared_ptr<Outgoing>> outgoing;
    bool pump_scheduled = false;
    std::string last_pumped;

    Impl(asio::io_context& context, ClientOptions settings, EventSink sink)
        : io(context), options(std::move(settings)), events(std::move(sink)), resolver(io), connect_timer(io) {
        if (!simple_name(options.name)) throw std::runtime_error("Choose a nickname with 1-48 bytes and no spaces");
        configure_tls(tls);
    }
    void emit(Json value) { if (events) events(std::move(value)); }
    void cleanup(const std::string& reason) {
        for (const auto& item : incoming) emit({{"type", "file_cancelled"}, {"transfer", item.first},
            {"offer", item.second->offer}, {"reason", item.second->saved ? "File saved; final confirmation was lost" : reason}});
        for (const auto& item : outgoing) emit({{"type", "file_cancelled"}, {"transfer", item.first},
            {"offer", item.second->offer}, {"reason", reason}});
        incoming.clear(); outgoing.clear(); accepted.clear(); offers.clear(); sources.clear();
    }
    void close() {
        ++generation;
        resolver.cancel(); connect_timer.cancel();
        ready = false;
        if (connection) connection->close("Disconnected");
        connection.reset();
        cleanup("Disconnected");
    }
    void connect() {
        close();
        const auto current = generation;
        self_id.clear();
        connection = std::make_shared<Connection>(io, tls);
        auto peer = connection;
        std::weak_ptr<Impl> weak = shared_from_this();
        peer->on_ready = [weak, current] {
            if (auto self = weak.lock(); self && self->generation == current) {
                self->connect_timer.cancel();
                self->connection->send(Json{{"type", "hello"}, {"version", protocol_version},
                    {"name", self->options.name}, {"secret", self->options.invitation.secret}});
            }
        };
        peer->on_json = [weak, current](const Json& value) {
            if (auto self = weak.lock(); self && self->generation == current) self->receive(value);
        };
        peer->on_chunk = [weak, current](Packet packet) {
            if (auto self = weak.lock(); self && self->generation == current) self->binary(std::move(packet));
        };
        peer->on_close = [weak, current](const std::string& reason) {
            if (auto self = weak.lock(); self && self->generation == current) {
                self->connect_timer.cancel(); self->resolver.cancel(); self->ready = false;
                self->cleanup(reason);
                self->emit({{"type", "disconnected"}, {"reason", reason}});
            }
        };
        emit({{"type", "connecting"}, {"host", options.invitation.host}, {"port", options.invitation.port}});
        connect_timer.expires_after(std::chrono::seconds(15));
        connect_timer.async_wait([weak, current](const asio::error_code& error) {
            if (auto self = weak.lock(); self && !error && self->generation == current) {
                self->resolver.cancel(); self->connection->close("Connection or DNS lookup timed out");
            }
        });
        resolver.async_resolve(options.invitation.host, std::to_string(options.invitation.port),
            [weak, peer, current](const asio::error_code& error, Tcp::resolver::results_type endpoints) {
                const auto self = weak.lock();
                if (!self || self->generation != current || peer->closed()) return;
                if (error) { peer->close("Address lookup failed: " + error.message()); return; }
                asio::async_connect(peer->socket(), endpoints,
                    [weak, peer, current](const asio::error_code& connect_error, const Tcp::endpoint&) {
                        const auto owner = weak.lock();
                        if (!owner || owner->generation != current || peer->closed()) return;
                        if (connect_error) { peer->close("Connection failed: " + connect_error.message()); return; }
                        peer->start(false, owner->options.invitation.pin);
                    });
            });
    }
    void command(Json value) {
        try {
            const auto type = string_field(value, "type", 32);
            if (type == "reconnect") { connect(); return; }
            if (type == "disconnect") { close(); emit({{"type", "disconnected"}, {"reason", "Disconnected; use /reconnect"}}); return; }
            if (!ready || !connection || connection->closed()) throw std::runtime_error("Not connected; use /reconnect");
            if (type == "file_offer") prepare_offer(value);
            else if (type == "file_accept") accept_offer(value);
            else if (type == "file_decline") {
                const auto id = string_field(value, "offer", 32);
                connection->send(Json{{"type", "file_decline"}, {"offer", id}});
            } else if (type == "file_cancel") {
                cancel_transfer(string_field(value, "transfer", 32), "Cancelled locally");
            } else if (type == "message") {
                const auto conversation = string_field(value, "conversation", 64);
                const auto text = string_field(value, "text", max_text);
                const auto request = value.contains("request") ? string_field(value, "request", 64) : random_hex();
                if (connection->send(Json{{"type", "message"}, {"conversation", conversation}, {"text", text}, {"request", request}}))
                    emit({{"type", "message_pending"}, {"conversation", conversation}, {"text", text},
                          {"request", request}, {"name", options.name}, {"sender", self_id}, {"time", now_ms()}});
            } else connection->send(value);
        } catch (const std::exception& error) {
            emit({{"type", "error"}, {"message", error.what()}});
        }
    }
    void prepare_offer(const Json& value) {
        if (sources.size() >= 8) throw std::runtime_error("At most 8 outgoing offers may be pending");
        const auto path = fs::u8path(string_field(value, "path", 4096));
        if (!fs::is_regular_file(path)) throw std::runtime_error("Choose an existing regular file");
        const auto size = fs::file_size(path);
        if (size > max_send_file) throw std::runtime_error("File exceeds the server's size limit");
        const auto filename = path.filename().u8string();
        if (!safe_filename(filename)) throw std::runtime_error("Unsupported file name");
        const auto conversation = string_field(value, "conversation", 64);
        const auto id = random_hex();
        sources.emplace(id, Source{path, size});
        connection->send(Json{{"type", "file_offer"}, {"offer", id}, {"conversation", conversation},
                              {"filename", filename}, {"size", size}});
        emit({{"type", "file_queued"}, {"offer", id}, {"conversation", conversation}, {"filename", filename},
              {"size", size}, {"direction", "out"}});
    }
    void accept_offer(const Json& value) {
        const auto id = string_field(value, "offer", 32);
        const auto found = offers.find(id);
        if (found == offers.end() || accepted.count(id)) throw std::runtime_error("Offer is unavailable or already accepted");
        if (incoming.size() + accepted.size() >= 8) throw std::runtime_error("At most 8 files can be received at once");
        const auto size = uint_field(found->second, "size", options.max_file);
        const auto filename = string_field(found->second, "filename", 240);
        if (!safe_filename(filename)) throw std::runtime_error("Unsafe incoming file name");
        fs::path destination;
        if (value.contains("path")) destination = fs::u8path(string_field(value, "path", 4096));
        else {
            private_directory(options.downloads);
            destination = options.downloads / fs::u8path(filename);
        }
        auto item = std::make_shared<Incoming>();
        item->offer = id; item->filename = filename; item->size = size;
        (void)string_field(found->second, "conversation", 64);
        item->file = std::make_unique<IncomingFile>(destination);
        accepted.emplace(id, item);
        connection->send(Json{{"type", "file_accept"}, {"offer", id}});
        emit({{"type", "file_accepting"}, {"offer", id}, {"path", destination.u8string()}});
    }
    void progress(const std::string& id, const std::string& offer, std::uint64_t done, std::uint64_t size,
                  std::chrono::steady_clock::time_point& last) {
        const auto now = std::chrono::steady_clock::now();
        if (done != size && now - last < std::chrono::milliseconds(100)) return;
        last = now;
        emit({{"type", "file_progress"}, {"transfer", id}, {"offer", offer}, {"done", done}, {"size", size}});
    }
    void cancel_transfer(const std::string& id, const std::string& reason) {
        if (connection && !connection->closed()) connection->send(Json{{"type", "file_cancel"}, {"transfer", id}});
        incoming.erase(id); outgoing.erase(id);
        schedule_pump();
        emit({{"type", "file_cancelled"}, {"transfer", id}, {"reason", reason}});
    }
    void begin(const Json& value);
    void schedule_pump() {
        if (pump_scheduled) return;
        pump_scheduled = true;
        std::weak_ptr<Impl> weak = shared_from_this();
        asio::post(io, [weak] {
            const auto self = weak.lock();
            if (!self) return;
            self->pump_scheduled = false;
            std::vector<std::shared_ptr<Outgoing>> pending;
            auto start = self->outgoing.upper_bound(self->last_pumped);
            for (auto it = start; it != self->outgoing.end(); ++it) pending.push_back(it->second);
            for (auto it = self->outgoing.begin(); it != start; ++it) pending.push_back(it->second);
            for (const auto& item : pending) {
                if (!self->outgoing.count(item->id) || !self->ready) continue;
                const auto sent = item->sent;
                self->pump(item);
                if (item->sent != sent) self->last_pumped = item->id;
            }
        });
    }
    void pump(const std::shared_ptr<Outgoing>& item);
    void receive(const Json& value);
    void binary(Packet packet);
};

void Client::Impl::begin(const Json& value) {
    const auto id = string_field(value, "transfer", 32);
    const auto oid = string_field(value, "offer", 32);
    try {
        const auto sender = string_field(value, "sender", 32);
        if (sender == self_id) {
            const auto found = sources.find(oid);
            if (found == sources.end()) throw std::runtime_error("Source offer is no longer available");
            if (outgoing.size() >= 64) throw std::runtime_error("Too many simultaneous outgoing transfers");
            auto item = std::make_shared<Outgoing>();
            item->id = id; item->offer = oid; item->source = found->second;
            item->input.open(item->source.path, std::ios::binary | std::ios::ate);
            if (!item->input || item->input.tellg() < 0 || static_cast<std::uint64_t>(item->input.tellg()) != item->source.size)
                throw std::runtime_error("Source file changed or cannot be read");
            item->input.seekg(0);
            outgoing.emplace(id, item);
            auto event = value; event["direction"] = "out"; emit(event);
            schedule_pump();
        } else {
            const auto found = accepted.find(oid);
            if (found == accepted.end()) throw std::runtime_error("File was not explicitly accepted");
            auto item = found->second;
            if (uint_field(value, "size", options.max_file) != item->size || string_field(value, "filename", 240) != item->filename)
                throw std::runtime_error("Offer metadata changed");
            accepted.erase(found); offers.erase(oid);
            item->id = id;
            incoming.emplace(id, item);
            auto event = value; event["direction"] = "in"; event["path"] = item->file->path().u8string(); emit(event);
        }
    } catch (const std::exception& error) { accepted.erase(oid); cancel_transfer(id, error.what()); }
}
void Client::Impl::pump(const std::shared_ptr<Outgoing>& item) {
    try {
        std::array<unsigned char, chunk_size> buffer{};
        std::uint64_t outstanding = 0;
        for (const auto& transfer : outgoing) outstanding += transfer.second->sent - transfer.second->acknowledged;
        // Bound aggregate queued data when a room has many accepting recipients.
        constexpr std::uint64_t connection_window = 2 * transfer_window;
        while (!item->ended && item->sent < item->source.size && item->sent - item->acknowledged < transfer_window &&
               outstanding < connection_window) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>({chunk_size,
                item->source.size - item->sent, transfer_window - (item->sent - item->acknowledged), connection_window - outstanding}));
            if (!item->input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count)))
                throw std::runtime_error("Source file was truncated or could not be read");
            item->hash.update(buffer.data(), count);
            const auto packet = chunk_packet(item->id, item->sent, buffer.data(), count);
            item->sent += count;
            outstanding += count;
            if (!connection->send(packet)) return;
        }
        if (!item->ended && item->acknowledged == item->source.size) {
            if (fs::file_size(item->source.path) != item->source.size || item->input.peek() != std::char_traits<char>::eof() || item->input.bad())
                throw std::runtime_error("Source file changed during transfer");
            item->ended = true;
            connection->send(Json{{"type", "file_end"}, {"transfer", item->id}, {"sha256", item->hash.finish()}});
        }
    } catch (const std::exception& error) { cancel_transfer(item->id, error.what()); }
}
void Client::Impl::binary(Packet packet) {
    const auto chunk = parse_chunk(packet);
    const auto found = incoming.find(chunk.id);
    if (found == incoming.end()) return; // Already cancelled; discard bounded in-flight chunks.
    const auto item = found->second;
    try {
        if (item->saved || chunk.offset != item->received || chunk.size > item->size - item->received)
            throw std::runtime_error("Unexpected file offset or size");
        item->file->write(chunk.data, chunk.size);
        item->hash.update(chunk.data, chunk.size);
        item->received += chunk.size;
        connection->send(Json{{"type", "file_ack"}, {"transfer", item->id}, {"offset", item->received}});
        progress(item->id, item->offer, item->received, item->size, item->progress);
    } catch (const std::exception& error) { cancel_transfer(item->id, error.what()); }
}
void Client::Impl::receive(const Json& value) {
    const auto type = string_field(value, "type", 32);
    if (type == "ping") { connection->send(Json{{"type", "pong"}}); return; }
    if (type == "pong") return;
    if (type == "welcome") {
        self_id = string_field(value, "self", 32);
        max_send_file = uint_field(value, "max_file", std::numeric_limits<std::uint64_t>::max());
        ready = true;
    } else if (type == "file_offer") {
        const auto id = string_field(value, "offer", 32);
        if (offers.size() >= 256) { connection->send(Json{{"type", "file_decline"}, {"offer", id}}); return; }
        offers[id] = value;
    } else if (type == "file_begin") { begin(value); return; }
    else if (type == "file_ack") {
        const auto id = string_field(value, "transfer", 32);
        const auto found = outgoing.find(id);
        if (found == outgoing.end()) return;
        const auto item = found->second;
        const auto offset = uint_field(value, "offset", item->sent);
        if (offset < item->acknowledged) throw std::runtime_error("Invalid file acknowledgement");
        item->acknowledged = offset;
        progress(id, item->offer, offset, item->source.size, item->progress);
        schedule_pump();
        return;
    } else if (type == "file_end") {
        const auto id = string_field(value, "transfer", 32);
        const auto found = incoming.find(id);
        if (found == incoming.end()) return;
        const auto item = found->second;
        try {
            if (item->received != item->size) throw std::runtime_error("File ended early");
            const auto actual = item->hash.finish();
            if (!constant_equal(actual, string_field(value, "sha256", 64))) throw std::runtime_error("SHA-256 verification failed");
            item->file->commit(); item->saved = true;
            connection->send(Json{{"type", "file_done"}, {"transfer", id}, {"sha256", actual}});
            emit({{"type", "file_saved"}, {"transfer", id}, {"offer", item->offer},
                {"path", item->file->path().u8string()}, {"sha256", actual}, {"size", item->size}});
        } catch (const std::exception& error) { cancel_transfer(id, error.what()); }
        return;
    } else if (type == "file_complete" || type == "file_cancelled") {
        const auto id = string_field(value, "transfer", 32);
        incoming.erase(id); outgoing.erase(id);
        schedule_pump();
    } else if (type == "file_offer_closed") {
        const auto id = string_field(value, "offer", 32);
        offers.erase(id); sources.erase(id); accepted.erase(id);
    } else if (type == "file_declined") {
        if (value.value("peer", "") == self_id) offers.erase(string_field(value, "offer", 32));
    } else if (type == "error") {
        if (value.contains("offer")) {
            const auto id = string_field(value, "offer", 32);
            sources.erase(id); accepted.erase(id);
        }
        if (value.contains("transfer")) {
            const auto id = string_field(value, "transfer", 32);
            cancel_transfer(id, value.value("message", "Transfer rejected"));
        }
    }
    emit(value);
}
Client::Client(asio::io_context& io, ClientOptions options, EventSink events)
    : impl_(std::make_shared<Impl>(io, std::move(options), std::move(events))) {}
void Client::connect() { impl_->connect(); }
void Client::command(Json value) { impl_->command(std::move(value)); }
void Client::close() { impl_->close(); }

Application::Application(AppOptions options, EventSink events) : events_(std::move(events)) {
    if (options.client.downloads.empty()) options.client.downloads = options.data_directory / "downloads";
    if (options.host) {
        auto identity = load_identity(options.data_directory / "host");
        server_ = std::make_unique<Server>(io_, std::move(identity), options.server);
        invitation_ = server_->invitation().str();
        options.client.invitation = server_->invitation();
        options.client.invitation.host = options.server.bind == "0.0.0.0" ? "127.0.0.1" :
            (options.server.bind == "::" ? "::1" : options.server.bind);
    }
    client_ = std::make_unique<Client>(io_, std::move(options.client), events_);
    if (server_) server_->start();
    client_->connect();
    worker_ = std::thread([this] {
        try { io_.run(); }
        catch (const std::exception& error) {
            client_->close();
            if (server_) server_->stop();
            work_.reset();
            if (events_) events_({{"type", "disconnected"}, {"reason", std::string("Network error: ") + error.what()}});
            io_.run();
        }
    });
}
Application::~Application() { stop(); }
void Application::command(Json value) {
    asio::post(io_, [this, value = std::move(value)]() mutable { client_->command(std::move(value)); });
}
void Application::stop() {
    if (!worker_.joinable()) return;
    asio::post(io_, [this] { client_->close(); if (server_) server_->stop(); work_.reset(); });
    worker_.join();
}
} // namespace lemon
