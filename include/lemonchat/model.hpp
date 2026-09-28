#pragma once
#include "common.hpp"
#include <deque>
#include <map>
#include <vector>

namespace lemon {
struct MessageView {
    std::string request, sender, name, text, status;
    std::int64_t time = 0;
};
struct ConversationView {
    std::string name, draft;
    bool direct = false, joined = false;
    std::deque<MessageView> messages;
    std::size_t unread = 0;
};
struct TransferView {
    std::string id, offer, filename, path, peer, status, detail;
    std::uint64_t size = 0, done = 0;
    bool incoming = false, is_offer = false;
    std::int64_t started = 0;
};
class Model {
public:
    void apply(const Json& event);
    void select(const std::string& id);
    std::string peer_name(const std::string& id) const;
    std::string title(const ConversationView& conversation) const;
    std::vector<const MessageView*> search(const std::string& id, const std::string& query) const;
    Json transcript(const std::string& id) const;
    std::string find_peer(const std::string& name_or_id) const;
    std::string find_transfer(const std::string& prefix) const;
    std::string self, name, active = "lobby", status = "Starting";
    bool connected = false;
    std::map<std::string, std::string> peers;
    std::map<std::string, ConversationView> conversations;
    std::map<std::string, TransferView> transfers;
    std::vector<std::pair<std::string, std::string>> rooms;
private:
    void trim();
};
} // namespace lemon
