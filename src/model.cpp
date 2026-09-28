#include "lemonchat/model.hpp"
#include <cctype>

namespace lemon {
namespace {
std::string lowercase(std::string value) {
    for (char& ch : value) if (static_cast<unsigned char>(ch) < 128) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
}
void Model::select(const std::string& id) {
    if (!conversations.count(id)) return;
    active = id;
    conversations[id].unread = 0;
}
std::string Model::peer_name(const std::string& id) const {
    const auto found = peers.find(id);
    return found == peers.end() ? id.substr(0, 8) : found->second;
}
std::string Model::title(const ConversationView& conversation) const {
    if (!conversation.direct) return "#" + conversation.name;
    return "@" + conversation.name;
}
void Model::apply(const Json& event) {
    const auto type = event.value("type", "");
    if (type == "connecting") { connected = false; status = "Connecting to " + event.value("host", ""); }
    else if (type == "welcome") {
        self = event.at("self"); name = terminal_text(event.at("name")); connected = true; status = "Connected · TLS 1.3 · trusted host";
    } else if (type == "disconnected") {
        connected = false; status = event.value("reason", "Disconnected");
        for (auto& entry : conversations) for (auto& message : entry.second.messages)
            if (message.status == "pending") message.status = "unconfirmed";
        for (auto& entry : transfers)
            if (entry.second.is_offer && (entry.second.status == "offered" || entry.second.status == "accepting" || entry.second.status == "waiting"))
                entry.second.status = "closed";
    } else if (type == "state") {
        peers.clear(); rooms.clear();
        for (const auto& peer : event.at("peers")) peers[peer.at("id")] = terminal_text(peer.at("name"));
        for (const auto& room : event.at("rooms")) rooms.emplace_back(room.at("id"), terminal_text(room.at("name")));
        for (auto& entry : conversations) entry.second.joined = false;
        for (const auto& item : event.at("conversations")) {
            const std::string id = item.at("id");
            auto& conversation = conversations[id];
            conversation.name = terminal_text(item.at("name"));
            conversation.direct = item.at("direct"); conversation.joined = true;
            if (conversation.direct) for (const auto& member : item.at("members"))
                if (member != self) conversation.name = peer_name(member.get<std::string>());
        }
        if (!conversations.count(active) && !conversations.empty()) select(conversations.begin()->first);
        for (auto it = conversations.begin(); it != conversations.end();)
            if (!it->second.joined && it->second.messages.empty() && it->second.draft.empty() && it->first != active)
                it = conversations.erase(it);
            else ++it;
    } else if (type == "conversation_opened") select(event.at("conversation"));
    else if (type == "message" || type == "message_pending") {
        const std::string id = event.at("conversation");
        auto& conversation = conversations[id];
        const auto request = event.value("request", "");
        MessageView message{request, event.value("sender", ""),
            terminal_text(event.value("name", "")), terminal_text(event.value("text", "")),
            type == "message_pending" ? "pending" : "sent", event.value("time", now_ms())};
        bool replaced = false;
        if (type == "message" && message.sender == self) {
            for (auto& existing : conversation.messages) if (existing.request == request && existing.sender == self) {
                existing = message; replaced = true; break;
            }
        }
        if (!replaced) {
            conversation.messages.push_back(std::move(message));
            if (id != active) ++conversation.unread;
        }
        trim();
    } else if (type == "error") {
        status = "Error: " + terminal_text(event.value("message", "Unknown error"));
        const auto request = event.value("request", "");
        if (!request.empty()) for (auto& entry : conversations) for (auto& message : entry.second.messages)
            if (message.request == request && message.sender == self) message.status = "failed";
        const auto offer = event.value("offer", "");
        if (transfers.count(offer)) { transfers[offer].status = "failed"; transfers[offer].detail = status; }
    } else if (type == "file_offer" || type == "file_queued") {
        const std::string id = event.at("offer");
        auto& file = transfers[id];
        file.id = id; file.offer = id; file.is_offer = true; file.incoming = type == "file_offer";
        file.filename = terminal_text(event.at("filename"));
        file.size = event.at("size"); file.status = file.incoming ? "offered" : "waiting";
        file.started = now_ms(); file.peer = terminal_text(event.value("name", "Room members"));
        if (file.incoming) status = "File offered: " + file.filename + " — open Files to accept or decline";
    } else if (type == "file_accepting") {
        auto& file = transfers[event.at("offer")]; file.status = "accepting"; file.path = event.value("path", "");
    } else if (type == "file_begin") {
        const std::string id = event.at("transfer"), oid = event.at("offer");
        auto& file = transfers[id];
        file.id = id; file.offer = oid; file.is_offer = false; file.incoming = event.value("direction", "in") == "in";
        file.filename = terminal_text(event.at("filename")); file.size = event.at("size");
        file.status = "transferring"; file.started = now_ms();
        file.peer = file.incoming ? peer_name(event.value("sender", "")) : terminal_text(event.value("receiver_name", ""));
        file.path = event.value("path", "");
        if (transfers.count(oid) && transfers[oid].incoming) transfers[oid].status = "accepted";
    } else if (type == "file_progress") {
        const std::string id = event.at("transfer");
        if (transfers.count(id)) { transfers[id].done = event.at("done"); transfers[id].size = event.at("size"); }
    } else if (type == "file_saved" || type == "file_complete" || type == "file_cancelled") {
        const std::string id = event.at("transfer");
        if (transfers.count(id)) {
            auto& file = transfers[id];
            file.status = type == "file_complete" ? "complete" : (type == "file_saved" ? "saved" : "cancelled");
            file.detail = terminal_text(event.value("reason", ""));
            if (type == "file_saved") file.path = event.at("path");
            if (type == "file_complete") file.done = file.size;
            const auto parent = transfers.find(file.offer);
            if (parent != transfers.end() && parent->second.incoming) parent->second.status = file.status;
        }
    } else if (type == "file_declined" || type == "file_offer_closed") {
        const std::string id = event.at("offer");
        if (transfers.count(id)) {
            if (type == "file_offer_closed" || event.value("peer", "") == self)
                transfers[id].status = type == "file_declined" ? "declined" : "closed";
            transfers[id].detail = terminal_text(event.value("reason", "A recipient declined"));
        }
    }
    // Keep completed transfer metadata bounded without evicting active transfers.
    while (transfers.size() > 512) {
        auto oldest = transfers.end();
        for (auto it = transfers.begin(); it != transfers.end(); ++it) {
            const auto& file_status = it->second.status;
            if (file_status != "complete" && file_status != "cancelled" && file_status != "declined" && file_status != "closed" && file_status != "failed") continue;
            if (oldest == transfers.end() || it->second.started < oldest->second.started) oldest = it;
        }
        if (oldest == transfers.end()) break;
        transfers.erase(oldest);
    }
}
void Model::trim() {
    std::size_t count = 0, bytes = 0;
    for (const auto& entry : conversations) for (const auto& message : entry.second.messages) { ++count; bytes += message.text.size(); }
    while (count > 2000 || bytes > 8 * 1024 * 1024) {
        auto oldest = conversations.end();
        for (auto it = conversations.begin(); it != conversations.end(); ++it) {
            if (it->second.messages.empty()) continue;
            if (oldest == conversations.end() || it->second.messages.front().time < oldest->second.messages.front().time) oldest = it;
        }
        if (oldest == conversations.end()) break;
        bytes -= oldest->second.messages.front().text.size(); --count;
        oldest->second.messages.pop_front();
    }
}
std::vector<const MessageView*> Model::search(const std::string& id, const std::string& query) const {
    std::vector<const MessageView*> result;
    const auto found = conversations.find(id);
    if (found == conversations.end()) return result;
    const auto needle = lowercase(query);
    for (const auto& message : found->second.messages)
        if (needle.empty() || lowercase(message.text).find(needle) != std::string::npos || lowercase(message.name).find(needle) != std::string::npos)
            result.push_back(&message);
    return result;
}
Json Model::transcript(const std::string& id) const {
    Json messages = Json::array();
    if (const auto found = conversations.find(id); found != conversations.end())
        for (const auto& message : found->second.messages) messages.push_back({{"name", message.name}, {"text", message.text},
            {"time", message.time}, {"status", message.status}});
    return {{"conversation", id}, {"messages", messages}};
}
std::string Model::find_peer(const std::string& name_or_id) const {
    for (const auto& peer : peers) if (peer.first == name_or_id || peer.second == name_or_id) return peer.first;
    throw std::runtime_error("No online member named " + name_or_id);
}
std::string Model::find_transfer(const std::string& prefix) const {
    if (prefix.empty()) throw std::runtime_error("Specify a transfer ID");
    std::string match;
    for (const auto& file : transfers) if (file.first.compare(0, prefix.size(), prefix) == 0) {
        if (!match.empty()) throw std::runtime_error("Transfer ID prefix is ambiguous");
        match = file.first;
    }
    if (match.empty()) throw std::runtime_error("Transfer not found");
    return match;
}
} // namespace lemon
