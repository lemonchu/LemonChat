#include "lemonchat/server.hpp"
#include <map>
#include <set>
#include <deque>

namespace lemon {
struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    struct Session {
        std::string id, name;
        std::shared_ptr<Connection> connection;
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point rate_start = created;
        unsigned commands = 0;
        std::set<std::string> recent;
        std::deque<std::string> recent_order;
    };
    struct Conversation {
        std::string id, name;
        bool direct = false;
        std::set<std::string> members;
    };
    struct Offer {
        std::string id, sender, conversation, filename;
        std::uint64_t size = 0;
        std::set<std::string> pending;
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    };
    struct Transfer {
        std::string id, receiver;
        std::shared_ptr<Offer> offer;
        std::uint64_t sent = 0, acknowledged = 0;
        bool ended = false;
        std::string hash;
        std::chrono::steady_clock::time_point updated = std::chrono::steady_clock::now();
    };
    Identity identity;
    ServerOptions options;
    asio::ssl::context tls{asio::ssl::context::tls_server};
    Tcp::acceptor acceptor;
    asio::steady_timer timer;
    bool stopped = false;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::map<std::string, Conversation> conversations;
    std::map<std::string, std::shared_ptr<Offer>> offers;
    std::map<std::string, std::shared_ptr<Transfer>> transfers;

    Impl(asio::io_context& context, Identity credentials, ServerOptions settings)
        : identity(std::move(credentials)), options(std::move(settings)), acceptor(context), timer(context) {
        configure_tls(tls);
        tls.use_certificate_chain(asio::buffer(identity.certificate));
        tls.use_private_key(asio::buffer(identity.private_key), asio::ssl::context::pem);
        if (SSL_CTX_check_private_key(tls.native_handle()) != 1) throw std::runtime_error("TLS identity key does not match its certificate");
        const Tcp::endpoint endpoint(asio::ip::make_address(options.bind), options.port);
        acceptor.open(endpoint.protocol());
#ifndef _WIN32
        acceptor.set_option(Tcp::acceptor::reuse_address(true));
#endif
        acceptor.bind(endpoint);
        acceptor.listen();
        options.port = acceptor.local_endpoint().port();
        Invitation::parse(invitation().str());
        conversations.emplace("lobby", Conversation{"lobby", "lobby", false, {}});
    }
    Invitation invitation() const { return {options.advertise, options.port, identity.pin, identity.secret}; }
    std::shared_ptr<Session> session(const std::string& id) const {
        const auto found = sessions.find(id);
        return found == sessions.end() ? nullptr : found->second;
    }
    void send(const std::string& id, const Json& value) {
        if (auto peer = session(id)) peer->connection->send(value);
    }
    void start() { accept(); maintenance(); }
    void accept() {
        if (stopped) return;
        acceptor.async_accept([self = shared_from_this()](const asio::error_code& error, Tcp::socket socket) {
            if (self->stopped) return;
            if (!error) {
                if (self->sessions.size() >= self->options.max_clients) {
                    asio::error_code ignored; socket.close(ignored);
                } else self->attach(std::move(socket));
            }
            self->accept();
        });
    }
    void attach(Tcp::socket socket) {
        auto peer = std::make_shared<Session>();
        peer->id = random_hex();
        peer->connection = std::make_shared<Connection>(std::move(socket), tls);
        sessions.emplace(peer->id, peer);
        std::weak_ptr<Impl> weak = shared_from_this();
        const auto id = peer->id;
        peer->connection->on_json = [weak, id](const Json& value) {
            if (auto self = weak.lock()) self->receive(id, value);
        };
        peer->connection->on_chunk = [weak, id](Packet packet) {
            if (auto self = weak.lock()) self->binary(id, std::move(packet));
        };
        peer->connection->on_close = [weak, id](const std::string&) {
            if (auto self = weak.lock()) self->drop(id);
        };
        peer->connection->start(true);
    }
    void state(bool validate_only = false) {
        Json peers = Json::array(), rooms = Json::array();
        std::vector<std::pair<std::shared_ptr<Session>, Packet>> packets;
        for (const auto& item : sessions) if (!item.second->name.empty()) {
            peers.push_back({{"id", item.first}, {"name", item.second->name}});
        }
        for (const auto& item : conversations) if (!item.second.direct)
            rooms.push_back({{"id", item.first}, {"name", item.second.name}, {"count", item.second.members.size()}});
        for (const auto& entry : sessions) {
            const auto& peer = entry.second;
            if (peer->name.empty()) continue;
            Json joined = Json::array();
            for (const auto& item : conversations) if (item.second.members.count(peer->id)) {
                const auto& conversation = item.second;
                joined.push_back({{"id", item.first}, {"name", conversation.name},
                    {"direct", conversation.direct}, {"members", conversation.members}});
            }
            packets.emplace_back(peer, json_packet(Json{{"type", "state"}, {"self", peer->id}, {"peers", peers},
                                        {"rooms", rooms}, {"conversations", joined}}));
        }
        if (!validate_only) for (const auto& entry : packets) entry.first->connection->send(entry.second);
    }
    Conversation conversation_for(const std::string& id, const Json& value) const {
        const auto cid = string_field(value, "conversation", 64);
        const auto found = conversations.find(cid);
        if (found == conversations.end() || !found->second.members.count(id))
            throw std::runtime_error("You are not a member of that conversation");
        return found->second;
    }
    void hello(const std::shared_ptr<Session>& peer, const Json& value) {
        if (string_field(value, "type") != "hello" || uint_field(value, "version", 100) != protocol_version ||
            !constant_equal(string_field(value, "secret", 64), identity.secret))
            throw std::runtime_error("Invitation rejected or protocol version mismatch");
        const auto name = string_field(value, "name", 48);
        if (!simple_name(name)) throw std::runtime_error("Nickname must contain 1-48 bytes without spaces or control characters");
        for (const auto& entry : sessions) if (entry.second->name == name) throw std::runtime_error("Nickname is already in use");
        peer->name = name;
        conversations.at("lobby").members.insert(peer->id);
        try { state(true); }
        catch (...) { peer->name.clear(); conversations.at("lobby").members.erase(peer->id); throw; }
        peer->connection->send(Json{{"type", "welcome"}, {"self", peer->id}, {"name", name},
            {"version", protocol_version}, {"max_file", options.max_file}});
        state();
    }
    void receive(const std::string& id, const Json& value) {
        auto peer = session(id);
        if (!peer) return;
        try {
            if (peer->name.empty()) { hello(peer, value); return; }
            const auto type = string_field(value, "type", 32);
            if (type == "ping") { peer->connection->send(Json{{"type", "pong"}}); return; }
            if (type == "pong") return;
            if (type != "file_ack") {
                const auto now = std::chrono::steady_clock::now();
                if (now - peer->rate_start >= std::chrono::seconds(1)) { peer->commands = 0; peer->rate_start = now; }
                if (++peer->commands > 100) throw std::runtime_error("Too many commands; slow down");
            }
            if (type == "message") message(peer, value);
            else if (type == "room_create" || type == "room_join" || type == "room_leave" || type == "direct") membership(peer, value, type);
            else if (type == "file_offer") offer(peer, value);
            else if (type == "file_accept" || type == "file_decline") decide(peer, value, type == "file_accept");
            else if (type == "file_ack" || type == "file_end" || type == "file_done" || type == "file_cancel") transfer_control(peer, value, type);
            else if (type == "offer_cancel") {
                const auto oid = string_field(value, "offer", 32);
                const auto found = offers.find(oid);
                if (found == offers.end() || found->second->sender != id) throw std::runtime_error("Unknown outgoing offer");
                cancel_offer(oid, "Sender cancelled the offer");
            } else if (type == "state") state();
            else throw std::runtime_error("Unknown command: " + type);
        } catch (const std::exception& error) {
            Json reply{{"type", "error"}, {"message", terminal_text(error.what())}};
            for (const auto* field : {"request", "offer", "transfer"})
                if (value.contains(field) && value[field].is_string()) reply[field] = value[field].get<std::string>().substr(0, 64);
            peer->connection->send(reply);
            if (peer->name.empty()) peer->connection->finish("Authentication rejected");
        }
    }
    void message(const std::shared_ptr<Session>& peer, const Json& value) {
        const auto conversation = conversation_for(peer->id, value);
        const auto body = string_field(value, "text", max_text);
        const auto request = string_field(value, "request", 64);
        if (!peer->recent.insert(request).second) return;
        peer->recent_order.push_back(request);
        if (peer->recent_order.size() > 256) { peer->recent.erase(peer->recent_order.front()); peer->recent_order.pop_front(); }
        const Json event{{"type", "message"}, {"id", random_hex()}, {"conversation", conversation.id},
            {"sender", peer->id}, {"name", peer->name}, {"text", body}, {"request", request}, {"time", now_ms()}};
        const auto packet = json_packet(event);
        for (const auto& id : conversation.members) if (auto recipient = session(id)) recipient->connection->send(packet);
    }
    void membership(const std::shared_ptr<Session>& peer, const Json& value, const std::string& type) {
        const auto previous = conversations;
        std::string opened;
        if (type == "direct") {
            const auto target = string_field(value, "peer", 32);
            const auto other = session(target);
            if (!other || other->name.empty() || target == peer->id) throw std::runtime_error("Choose another online member");
            const std::set<std::string> members{peer->id, target};
            for (const auto& item : conversations)
                if (item.second.direct && item.second.members == members) opened = item.first;
            if (opened.empty()) {
                if (conversations.size() >= 256) throw std::runtime_error("Conversation limit reached");
                opened = random_hex();
                conversations.emplace(opened, Conversation{opened, "Direct", true, members});
            }
        } else if (type == "room_leave") {
            const auto conversation = conversation_for(peer->id, value);
            if (conversation.direct) throw std::runtime_error("Direct conversations have fixed members");
            conversations.at(conversation.id).members.erase(peer->id);
            std::vector<std::string> cancelled;
            for (const auto& item : offers)
                if (item.second->conversation == conversation.id && item.second->sender == peer->id) cancelled.push_back(item.first);
            for (const auto& oid : cancelled) cancel_offer(oid, "Sender left the conversation");
            cancelled.clear();
            for (const auto& item : transfers)
                if (item.second->offer->conversation == conversation.id && item.second->receiver == peer->id)
                    cancelled.push_back(item.first);
            for (const auto& tid : cancelled) cancel_transfer(tid, "Recipient left the conversation");
            cancelled.clear();
            for (auto& item : offers) {
                if (item.second->conversation == conversation.id && item.second->pending.erase(peer->id)) {
                    cancelled.push_back(item.first);
                }
            }
            for (const auto& oid : cancelled) {
                send(peer->id, {{"type", "file_offer_closed"}, {"offer", oid}, {"reason", "You left the conversation"}});
                cleanup_offer(oid);
            }
        } else {
            const auto name = string_field(value, "name", 48);
            if (!simple_name(name)) throw std::runtime_error("Room names must have 1-48 bytes without spaces or control characters");
            for (const auto& item : conversations) if (!item.second.direct && item.second.name == name) opened = item.first;
            if (type == "room_create") {
                if (!opened.empty()) throw std::runtime_error("Room already exists; join it instead");
                if (conversations.size() >= 256) throw std::runtime_error("Conversation limit reached");
                opened = random_hex();
                conversations.emplace(opened, Conversation{opened, name, false, {}});
            } else if (opened.empty()) throw std::runtime_error("Room does not exist");
            conversations.at(opened).members.insert(peer->id);
        }
        try { state(); }
        catch (...) { conversations = previous; throw; }
        if (!opened.empty()) send(peer->id, {{"type", "conversation_opened"}, {"conversation", opened}});
    }

    void offer(const std::shared_ptr<Session>& peer, const Json& value);
    void decide(const std::shared_ptr<Session>& peer, const Json& value, bool accepted);
    void transfer_control(const std::shared_ptr<Session>& peer, const Json& value, const std::string& type);
    void binary(const std::string& id, Packet packet);
    void cancel_transfer(const std::string& id, const std::string& reason);
    void cancel_offer(const std::string& id, const std::string& reason);
    void cleanup_offer(const std::string& id);
    void drop(const std::string& id);
    void maintenance();
    void stop();
};
void Server::Impl::offer(const std::shared_ptr<Session>& peer, const Json& value) {
    const auto conversation = conversation_for(peer->id, value);
    auto item = std::make_shared<Offer>();
    item->id = string_field(value, "offer", 32);
    if (item->id.size() != 32 || item->id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid offer ID");
    if (offers.count(item->id) || offers.size() >= 256) throw std::runtime_error("Offer already exists or limit reached");
    std::size_t owned = 0;
    for (const auto& entry : offers) if (entry.second->sender == peer->id) ++owned;
    if (owned >= 8) throw std::runtime_error("At most 8 outstanding file offers are allowed");
    item->filename = string_field(value, "filename", 240);
    if (!safe_filename(item->filename)) throw std::runtime_error("Invalid file name");
    item->size = uint_field(value, "size", options.max_file);
    item->sender = peer->id;
    item->conversation = conversation.id;
    for (const auto& id : conversation.members) if (id != peer->id && session(id)) item->pending.insert(id);
    if (item->pending.empty()) throw std::runtime_error("No other members in this conversation");
    offers.emplace(item->id, item);
    const auto recipients = item->pending;
    for (const auto& id : recipients) send(id, {{"type", "file_offer"}, {"offer", item->id},
        {"conversation", item->conversation}, {"sender", peer->id}, {"name", peer->name},
        {"filename", item->filename}, {"size", item->size}});
    send(peer->id, {{"type", "file_offered"}, {"offer", item->id}, {"recipients", recipients.size()},
        {"conversation", item->conversation}, {"filename", item->filename}, {"size", item->size}});
}
void Server::Impl::decide(const std::shared_ptr<Session>& peer, const Json& value, bool accepted) {
    const auto oid = string_field(value, "offer", 32);
    const auto found = offers.find(oid);
    if (found == offers.end() || !found->second->pending.count(peer->id)) throw std::runtime_error("Offer is no longer available");
    const auto offer = found->second;
    const auto conversation = conversations.find(offer->conversation);
    if (conversation == conversations.end() || !conversation->second.members.count(peer->id))
        throw std::runtime_error("You are no longer a member of this conversation");
    if (!accepted) {
        offer->pending.erase(peer->id);
        const Json event{{"type", "file_declined"}, {"offer", oid}, {"peer", peer->id}, {"name", peer->name}};
        send(offer->sender, event); send(peer->id, event);
        cleanup_offer(oid);
        return;
    }
    std::size_t active = 0;
    for (const auto& entry : transfers) if (entry.second->receiver == peer->id || entry.second->offer->sender == peer->id) ++active;
    if (active >= 8 || transfers.size() >= 128) throw std::runtime_error("Active transfer limit reached");
    auto transfer = std::make_shared<Transfer>();
    transfer->id = random_hex(); transfer->receiver = peer->id; transfer->offer = offer;
    transfers.emplace(transfer->id, transfer);
    offer->pending.erase(peer->id);
    const Json event{{"type", "file_begin"}, {"transfer", transfer->id}, {"offer", oid},
        {"sender", offer->sender}, {"receiver", peer->id}, {"receiver_name", peer->name},
        {"conversation", offer->conversation}, {"filename", offer->filename}, {"size", offer->size}};
    send(peer->id, event); send(offer->sender, event);
}
void Server::Impl::binary(const std::string& id, Packet packet) {
    const auto chunk = parse_chunk(packet);
    const auto found = transfers.find(chunk.id);
    if (found == transfers.end() || found->second->offer->sender != id) {
        // A late chunk after cancellation is harmless, but must never be routed elsewhere.
        send(id, {{"type", "file_cancelled"}, {"transfer", chunk.id}, {"reason", "Transfer is not active"}});
        return;
    }
    const auto item = found->second;
    if (item->ended || chunk.offset != item->sent || chunk.size > item->offer->size - item->sent ||
        item->sent - item->acknowledged + chunk.size > transfer_window) {
        cancel_transfer(item->id, "Invalid file offset, length, or flow-control window"); return;
    }
    item->sent += chunk.size;
    item->updated = std::chrono::steady_clock::now();
    if (auto recipient = session(item->receiver)) recipient->connection->send(std::move(packet));
    else cancel_transfer(item->id, "Recipient disconnected");
}
void Server::Impl::transfer_control(const std::shared_ptr<Session>& peer, const Json& value, const std::string& type) {
    const auto id = string_field(value, "transfer", 32);
    const auto found = transfers.find(id);
    if (found == transfers.end()) {
        if (type == "file_cancel" || type == "file_ack") return;
        throw std::runtime_error("Transfer is not active");
    }
    const auto item = found->second;
    const bool sender = item->offer->sender == peer->id;
    const bool receiver = item->receiver == peer->id;
    if (!sender && !receiver) throw std::runtime_error("This transfer belongs to another conversation");
    item->updated = std::chrono::steady_clock::now();
    if (type == "file_cancel") { cancel_transfer(id, "Cancelled by " + peer->name); return; }
    if (type == "file_ack") {
        if (!receiver || item->ended) throw std::runtime_error("Unexpected file acknowledgement");
        const auto offset = uint_field(value, "offset", item->sent);
        if (offset < item->acknowledged) throw std::runtime_error("Acknowledgement moved backwards");
        item->acknowledged = offset;
        send(item->offer->sender, {{"type", "file_ack"}, {"transfer", id}, {"offset", offset}});
    } else if (type == "file_end") {
        if (!sender || item->ended || item->sent != item->offer->size || item->acknowledged != item->sent)
            throw std::runtime_error("File ended before all bytes were acknowledged");
        const auto hash = string_field(value, "sha256", 64);
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid SHA-256");
        item->hash = hash; item->ended = true;
        send(item->receiver, {{"type", "file_end"}, {"transfer", id}, {"sha256", hash}});
    } else if (type == "file_done") {
        if (!receiver || !item->ended || string_field(value, "sha256", 64) != item->hash)
            throw std::runtime_error("Invalid final file acknowledgement");
        const Json event{{"type", "file_complete"}, {"transfer", id}, {"offer", item->offer->id},
            {"sha256", item->hash}, {"size", item->offer->size}};
        transfers.erase(id);
        send(item->offer->sender, event); send(item->receiver, event);
        cleanup_offer(item->offer->id);
    }
}
void Server::Impl::cancel_transfer(const std::string& id, const std::string& reason) {
    const auto found = transfers.find(id);
    if (found == transfers.end()) return;
    const auto item = found->second;
    transfers.erase(found);
    const Json event{{"type", "file_cancelled"}, {"transfer", id}, {"offer", item->offer->id}, {"reason", reason}};
    send(item->offer->sender, event); send(item->receiver, event);
    cleanup_offer(item->offer->id);
}
void Server::Impl::cancel_offer(const std::string& id, const std::string& reason) {
    const auto found = offers.find(id);
    if (found == offers.end()) return;
    const auto item = found->second;
    offers.erase(found);
    std::vector<std::string> cancelled;
    for (const auto& entry : transfers) if (entry.second->offer->id == id) cancelled.push_back(entry.first);
    for (const auto& tid : cancelled) cancel_transfer(tid, reason);
    const Json event{{"type", "file_offer_closed"}, {"offer", id}, {"reason", reason}};
    send(item->sender, event);
    for (const auto& peer : item->pending) send(peer, event);
}
void Server::Impl::cleanup_offer(const std::string& id) {
    const auto found = offers.find(id);
    if (found == offers.end() || !found->second->pending.empty()) return;
    for (const auto& entry : transfers) if (entry.second->offer->id == id) return;
    const auto sender = found->second->sender;
    offers.erase(found);
    send(sender, {{"type", "file_offer_closed"}, {"offer", id}, {"reason", "All recipients finished or declined"}});
}
void Server::Impl::drop(const std::string& id) {
    const auto peer = session(id);
    if (!peer) return;
    sessions.erase(id);
    std::vector<std::string> cancelled;
    for (const auto& item : offers) if (item.second->sender == id) cancelled.push_back(item.first);
    for (const auto& oid : cancelled) cancel_offer(oid, "Sender disconnected");
    cancelled.clear();
    for (const auto& item : transfers) if (item.second->receiver == id) cancelled.push_back(item.first);
    for (const auto& tid : cancelled) cancel_transfer(tid, "Recipient disconnected");
    cancelled.clear();
    for (auto& item : offers) if (item.second->pending.erase(id)) cancelled.push_back(item.first);
    for (const auto& oid : cancelled) cleanup_offer(oid);
    for (auto it = conversations.begin(); it != conversations.end();) {
        it->second.members.erase(id);
        if (it->second.direct && it->second.members.size() < 2) it = conversations.erase(it);
        else ++it;
    }
    if (!stopped && !peer->name.empty()) state();
}
void Server::Impl::maintenance() {
    timer.expires_after(std::chrono::seconds(5));
    timer.async_wait([self = shared_from_this()](const asio::error_code& error) {
        if (error || self->stopped) return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<Session>> peers;
        for (const auto& item : self->sessions) peers.push_back(item.second);
        for (const auto& peer : peers) {
            if (peer->name.empty()) {
                if (now - peer->created > std::chrono::seconds(10)) peer->connection->close("Authentication timeout");
            } else peer->connection->send(Json{{"type", "ping"}});
        }
        std::vector<std::string> expired;
        for (const auto& item : self->transfers)
            if (now - item.second->updated > std::chrono::seconds(120)) expired.push_back(item.first);
        for (const auto& id : expired) self->cancel_transfer(id, "File transfer timed out");
        expired.clear();
        for (const auto& item : self->offers)
            if (!item.second->pending.empty() && now - item.second->created > std::chrono::minutes(10)) expired.push_back(item.first);
        for (const auto& id : expired) self->cancel_offer(id, "File offer expired");
        self->maintenance();
    });
}
void Server::Impl::stop() {
    if (stopped) return;
    stopped = true;
    timer.cancel();
    asio::error_code ignored;
    acceptor.cancel(ignored); acceptor.close(ignored);
    std::vector<std::shared_ptr<Session>> peers;
    for (const auto& item : sessions) peers.push_back(item.second);
    for (const auto& peer : peers) peer->connection->close("Server stopped");
    transfers.clear(); offers.clear(); sessions.clear();
}
Server::Server(asio::io_context& io, Identity identity, ServerOptions options)
    : impl_(std::make_shared<Impl>(io, std::move(identity), std::move(options))) {}
void Server::start() { impl_->start(); }
void Server::stop() { impl_->stop(); }
Invitation Server::invitation() const { return impl_->invitation(); }
} // namespace lemon
