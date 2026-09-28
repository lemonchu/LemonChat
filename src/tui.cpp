#include "lemonchat/tui.hpp"
#include "lemonchat/model.hpp"
#include "lemonchat/storage.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace lemon {
namespace {
using namespace ftxui;
const Color accent = Color::RGB(191, 222, 75);
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return "";
    value = value.substr(first, value.find_last_not_of(" \r\n\t") - first + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return value;
}
std::string bytes(std::uint64_t amount) {
    std::ostringstream out;
    if (amount < 1024) out << amount << " B";
    else if (amount < 1024 * 1024) out << std::fixed << std::setprecision(1) << amount / 1024.0 << " KiB";
    else out << std::fixed << std::setprecision(1) << amount / (1024.0 * 1024.0) << " MiB";
    return out.str();
}
std::string timestamp(std::int64_t millis) {
    const auto seconds = static_cast<std::time_t>(millis / 1000);
    const auto* time = std::localtime(&seconds);
    if (!time) return "";
    std::ostringstream out; out << std::put_time(time, "%H:%M"); return out.str();
}
Element message_element(const MessageView& message, const std::string& self) {
    const bool own = message.sender == self;
    Elements lines;
    lines.push_back(hbox({text(" " + message.name) | bold | color(own ? accent : Color::Cyan),
        text("  " + timestamp(message.time)) | dim, filler(),
        text(own ? (message.status == "sent" ? "sent " : message.status + " ") : "") | dim}));
    std::istringstream content(message.text);
    std::string line;
    while (std::getline(content, line)) lines.push_back(paragraph(" " + line));
    lines.push_back(text(""));
    return vbox(std::move(lines));
}

class Terminal {
public:
    Terminal(AppOptions options, bool form) : options_(std::move(options)), stage_(form ? 2 : 0),
        launch_name_(options_.client.name), launch_invite_(options_.client.invitation.port ? options_.client.invitation.str() : ""),
        launch_port_(std::to_string(options_.server.port)), launch_bind_(options_.server.bind), launch_advertise_(options_.server.advertise) {
        launch_mode_ = options_.host ? 0 : 1;
        if (form) launch_mode_ = 0;
        build_components();
        if (!form) start(false);
    }
    int run() {
        screen_.Loop(root_);
        application_.reset();
        return 0;
    }
private:
    AppOptions options_;
    App screen_ = App::Fullscreen();
    Model model_;
    std::unique_ptr<Application> application_;
    std::mutex events_mutex_;
    std::deque<Json> events_;
    std::size_t event_bytes_ = 0;
    bool overloaded_ = false;
    int stage_ = 0, tab_ = 0, selected_chat_ = 0, selected_peer_ = 0, selected_file_ = 0, selected_room_ = 0;
    int composer_cursor_ = 0, scroll_ = 0, launch_mode_ = 0;
    std::string composer_, search_, room_name_, launch_name_, launch_invite_, launch_port_, launch_bind_, launch_advertise_, launch_error_;
    std::string dialog_title_, dialog_text_, dialog_value_, dialog_error_;
    std::function<void(const std::string&)> dialog_action_;
    std::vector<std::string> chat_labels_, chat_ids_, peer_labels_, peer_ids_, file_labels_, file_ids_, room_labels_;
    std::vector<std::string> tabs_{"Chat", "Files", "Rooms", "Help"};
    std::vector<std::string> modes_{"Create a server", "Join with invitation"};
    Component root_, normal_, dialog_, launch_, input_, find_, chat_menu_, file_menu_, member_menu_, room_menu_;
    Component dialog_input_;

    void enqueue(Json value) {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            if (overloaded_) return;
            wake = events_.empty();
            event_bytes_ += value.dump(-1, ' ', false, Json::error_handler_t::replace).size();
            if (event_bytes_ > 8 * 1024 * 1024 || events_.size() >= 2048) {
                overloaded_ = true;
                events_.clear();
                events_.push_back({{"type", "error"}, {"message", "UI could not keep up; connection stopped"}});
            } else events_.push_back(std::move(value));
        }
        if (wake) screen_.PostEvent(Event::Custom);
    }
    void start(bool from_form) {
        try {
            if (from_form) {
                options_.host = launch_mode_ == 0;
                options_.client.name = trim(launch_name_);
                if (options_.host) {
                    if (launch_port_.empty() || launch_port_.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("Port must be an integer from 1 to 65535");
                    const auto port = std::stoul(launch_port_);
                    if (!port || port > 65535) throw std::runtime_error("Port must be from 1 to 65535");
                    options_.server.port = static_cast<std::uint16_t>(port);
                    options_.server.bind = trim(launch_bind_); options_.server.advertise = trim(launch_advertise_);
                } else options_.client.invitation = Invitation::parse(trim(launch_invite_));
            }
            application_ = std::make_unique<Application>(options_, [this](Json event) { enqueue(std::move(event)); });
            stage_ = 0; input_->TakeFocus();
        } catch (const std::exception& error) { launch_error_ = error.what(); stage_ = 2; }
    }
    void drain() {
        std::deque<Json> pending;
        bool overloaded = false;
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            pending.swap(events_); event_bytes_ = 0;
            overloaded = overloaded_; overloaded_ = false;
        }
        if (overloaded && application_) application_->command({{"type", "disconnect"}});
        for (const auto& event : pending) {
            const auto old_active = model_.active;
            if (model_.conversations.count(old_active)) model_.conversations[old_active].draft = composer_;
            model_.apply(event);
            if (model_.active != old_active) {
                composer_ = model_.conversations[model_.active].draft;
                composer_cursor_ = static_cast<int>(composer_.size()); scroll_ = 0; tab_ = 0;
            }
        }
        lists();
    }
    void lists() {
        const auto previous_file = selected_file_ >= 0 && selected_file_ < static_cast<int>(file_ids_.size()) ?
            file_ids_[static_cast<std::size_t>(selected_file_)] : "";
        const auto previous_peer = selected_peer_ >= 0 && selected_peer_ < static_cast<int>(peer_ids_.size()) ?
            peer_ids_[static_cast<std::size_t>(selected_peer_)] : "";
        chat_labels_.clear(); chat_ids_.clear();
        for (const auto& item : model_.conversations) {
            const auto& chat = item.second;
            if (!chat.joined && chat.messages.empty()) continue;
            chat_ids_.push_back(item.first);
            chat_labels_.push_back(model_.title(chat) + (chat.unread ? " [" + std::to_string(chat.unread) + "]" : "") +
                                   (chat.joined ? "" : " (left)"));
            if (item.first == model_.active) selected_chat_ = static_cast<int>(chat_ids_.size() - 1);
        }
        peer_labels_.clear(); peer_ids_.clear();
        for (const auto& peer : model_.peers) if (peer.first != model_.self) {
            peer_ids_.push_back(peer.first); peer_labels_.push_back(peer.second);
        }
        file_labels_.clear(); file_ids_.clear();
        for (const auto& entry : model_.transfers) file_ids_.push_back(entry.first);
        std::stable_sort(file_ids_.begin(), file_ids_.end(), [this](const std::string& a, const std::string& b) {
            return model_.transfers.at(a).started > model_.transfers.at(b).started;
        });
        for (const auto& id : file_ids_) {
            const auto& file = model_.transfers.at(id);
            file_labels_.push_back(id.substr(0, 8) + " " + (file.incoming ? "< " : "> ") + file.filename + "  " + file.status);
        }
        room_labels_.clear();
        for (const auto& room : model_.rooms) {
            const auto found = model_.conversations.find(room.first);
            room_labels_.push_back("#" + room.second + (found != model_.conversations.end() && found->second.joined ? " (joined)" : ""));
        }
        selected_peer_ = std::min(selected_peer_, std::max(0, static_cast<int>(peer_ids_.size()) - 1));
        selected_file_ = std::min(selected_file_, std::max(0, static_cast<int>(file_ids_.size()) - 1));
        selected_room_ = std::min(selected_room_, std::max(0, static_cast<int>(room_labels_.size()) - 1));
        const auto file = std::find(file_ids_.begin(), file_ids_.end(), previous_file);
        if (file != file_ids_.end()) selected_file_ = static_cast<int>(std::distance(file_ids_.begin(), file));
        const auto peer = std::find(peer_ids_.begin(), peer_ids_.end(), previous_peer);
        if (peer != peer_ids_.end()) selected_peer_ = static_cast<int>(std::distance(peer_ids_.begin(), peer));
    }
    void command(Json value) {
        if (!application_) { model_.status = "Start or join a server first"; return; }
        application_->command(std::move(value));
    }
    void switch_chat() {
        if (selected_chat_ < 0 || selected_chat_ >= static_cast<int>(chat_ids_.size())) return;
        if (model_.conversations.count(model_.active)) model_.conversations[model_.active].draft = composer_;
        model_.select(chat_ids_[static_cast<std::size_t>(selected_chat_)]);
        composer_ = model_.conversations[model_.active].draft; composer_cursor_ = static_cast<int>(composer_.size());
        scroll_ = 0; search_.clear();
    }
    void show_dialog(std::string title, std::string description, std::string initial,
                     std::function<void(const std::string&)> action) {
        dialog_title_ = std::move(title); dialog_text_ = std::move(description); dialog_value_ = std::move(initial);
        dialog_action_ = std::move(action); dialog_error_.clear(); stage_ = 1; dialog_input_->TakeFocus();
    }
    void send_file() {
        const auto conversation = model_.active;
        show_dialog("Offer a file", "Every recipient chooses whether to accept. Existing files are protected.", "",
            [this, conversation](const std::string& path) {
                command({{"type", "file_offer"}, {"conversation", conversation}, {"path", path}}); tab_ = 1;
            });
    }
    TransferView* selected_file() {
        if (selected_file_ < 0 || selected_file_ >= static_cast<int>(file_ids_.size())) return nullptr;
        return &model_.transfers.at(file_ids_[static_cast<std::size_t>(selected_file_)]);
    }
    void accept_file(bool choose_path) {
        const auto* file = selected_file();
        if (!file || !file->incoming || !file->is_offer || file->status != "offered") {
            model_.status = "Select an incoming offer first"; return;
        }
        const auto id = file->id;
        if (!choose_path) command({{"type", "file_accept"}, {"offer", id}});
        else show_dialog("Save incoming file", "Choose a new file path. Existing files will not be overwritten.",
            (options_.client.downloads / std::filesystem::u8path(file->filename)).u8string(),
            [this, id](const std::string& path) { command({{"type", "file_accept"}, {"offer", id}, {"path", path}}); });
    }
    void cancel_file(bool decline) {
        const auto* file = selected_file();
        if (!file) return;
        if (decline && file->incoming && file->is_offer)
            command({{"type", "file_decline"}, {"offer", file->id}});
        else if (file->is_offer && !file->incoming) command({{"type", "offer_cancel"}, {"offer", file->id}});
        else if (!file->is_offer) command({{"type", "file_cancel"}, {"transfer", file->id}});
    }
    void submit() {
        try {
            if (composer_.empty()) return;
            const auto content = composer_;
            if (content.front() != '/') {
                if (!model_.connected) throw std::runtime_error("Not connected; your draft is preserved");
                if (content.size() > max_text) throw std::runtime_error("Messages may contain at most 16 KiB of UTF-8 text");
                command({{"type", "message"}, {"conversation", model_.active}, {"text", content}});
            } else {
                std::istringstream tokens(content);
                std::string name, rest; tokens >> name; std::getline(tokens, rest); rest = trim(rest);
                if (name == "/quit") { screen_.Exit(); return; }
                else if (name == "/help") tab_ = 3;
                else if (name == "/files") tab_ = 1;
                else if (name == "/rooms") tab_ = 2;
                else if (name == "/search") { search_ = rest; scroll_ = 0; find_->TakeFocus(); }
                else if (name == "/room") command({{"type", "room_create"}, {"name", rest}});
                else if (name == "/join") command({{"type", "room_join"}, {"name", rest}});
                else if (name == "/leave") command({{"type", "room_leave"}, {"conversation", model_.active}});
                else if (name == "/dm") command({{"type", "direct"}, {"peer", model_.find_peer(rest)}});
                else if (name == "/send") { command({{"type", "file_offer"}, {"conversation", model_.active}, {"path", rest}}); tab_ = 1; }
                else if (name == "/reconnect") command({{"type", "reconnect"}});
                else if (name == "/save") {
                    write_new_file(std::filesystem::u8path(rest), model_.transcript(model_.active).dump(2));
                    model_.status = "Saved plaintext transcript to " + rest;
                } else if (name == "/accept" || name == "/decline" || name == "/cancel") {
                    std::istringstream arguments(rest); std::string prefix, path; arguments >> prefix;
                    std::getline(arguments, path); path = trim(path);
                    const auto id = model_.find_transfer(prefix);
                    const auto& file = model_.transfers.at(id);
                    if (name == "/accept") {
                        Json request{{"type", "file_accept"}, {"offer", id}};
                        if (!path.empty()) request["path"] = path;
                        command(request);
                    } else if (name == "/decline") command({{"type", "file_decline"}, {"offer", id}});
                    else if (file.is_offer) command({{"type", "offer_cancel"}, {"offer", id}});
                    else command({{"type", "file_cancel"}, {"transfer", id}});
                } else throw std::runtime_error("Unknown command; use /help");
            }
            composer_.clear(); composer_cursor_ = 0; scroll_ = 0;
            if (model_.conversations.count(model_.active)) model_.conversations[model_.active].draft.clear();
        } catch (const std::exception& error) { model_.status = error.what(); }
    }
    Element timeline() {
        const auto messages = model_.search(model_.active, search_);
        if (messages.empty()) return vbox({filler(), text(search_.empty() ? "Start a conversation." : "No matching messages.") | center | dim, filler()}) | flex;
        const auto end = messages.size() - std::min(messages.size() - 1, static_cast<std::size_t>(std::max(0, scroll_)));
        const auto begin = end > 60 ? end - 60 : 0;
        Elements rendered;
        for (auto i = begin; i < end; ++i) rendered.push_back(message_element(*messages[i], model_.self));
        rendered.back() = rendered.back() | focus;
        return vbox(std::move(rendered)) | vscroll_indicator | yframe | flex;
    }
    Element transfer_details() {
        const auto* file = selected_file();
        if (!file) return text("File offers appear here. Use Send file from a conversation.") | dim;
        const auto seconds = std::max(0.001, (now_ms() - file->started) / 1000.0);
        const auto rate = static_cast<std::uint64_t>(file->done / seconds);
        return vbox({text(file->filename) | bold, text("ID: " + file->id) | dim,
            text(std::string(file->incoming ? "From: " : "To: ") + file->peer),
            text("Status: " + file->status + (file->detail.empty() ? "" : " — " + file->detail)),
            gauge(file->size ? static_cast<float>(file->done) / static_cast<float>(file->size) : (file->status == "complete" ? 1.0f : 0.0f)) | color(accent),
            text(bytes(file->done) + " / " + bytes(file->size) + "  " + bytes(rate) + "/s"),
            paragraph(file->path.empty() ? "Acceptance is required. No existing file is overwritten." : "Saved to: " + terminal_text(file->path))});
    }
    void build_components();
};

void Terminal::build_components() {
    auto style = ButtonOption::Animated(accent);
    InputOption composition;
    composition.multiline = false;
    composition.cursor_position = &composer_cursor_;
    composition.on_enter = [this] { submit(); };
    input_ = Input(&composer_, "Message or /help", composition);
    InputOption search_option;
    search_option.multiline = false;
    search_option.on_change = [this] { scroll_ = 0; };
    find_ = Input(&search_, "Search this conversation", search_option);
    auto chats = MenuOption::Vertical();
    chats.on_change = [this] { switch_chat(); };
    chats.on_enter = [this] { switch_chat(); input_->TakeFocus(); };
    chat_menu_ = Menu(&chat_labels_, &selected_chat_, chats);
    auto peers = MenuOption::Vertical();
    peers.on_enter = [this] {
        if (selected_peer_ >= 0 && selected_peer_ < static_cast<int>(peer_ids_.size()))
            command({{"type", "direct"}, {"peer", peer_ids_[static_cast<std::size_t>(selected_peer_)]}});
    };
    member_menu_ = Menu(&peer_labels_, &selected_peer_, peers);
    auto send = Button("Send", [this] { submit(); input_->TakeFocus(); }, style);
    auto attach = Button("Send file", [this] { send_file(); }, style);
    auto leave = Button("Leave room", [this] { command({{"type", "room_leave"}, {"conversation", model_.active}}); }, style);
    auto actions = Container::Horizontal({send, attach, leave});
    auto chat_center = Container::Vertical({find_, input_, actions});
    auto chat = Renderer(Container::Horizontal({chat_menu_, chat_center, member_menu_}), [this, actions] {
        const auto found = model_.conversations.find(model_.active);
        const auto title = found == model_.conversations.end() ? "#lobby" : model_.title(found->second);
        auto conversation = vbox({hbox({text(" " + title) | bold | color(accent), filler(), text("TLS 1.3 ") | dim}),
            separator(), hbox({text(" Find: ") | dim, find_->Render() | flex}), separator(),
            timeline(), separator(), input_->Render() | border | size(HEIGHT, LESS_THAN, 5),
            actions->Render(), text(" Enter send  |  Ctrl+N newline  |  PgUp/PgDn history") | dim}) | flex;
        Elements panes;
        if (ftxui::Terminal::Size().dimx >= 80)
            panes.push_back(vbox({text(" CONVERSATIONS") | bold, separator(),
                chat_menu_->Render() | vscroll_indicator | frame | flex}) | size(WIDTH, EQUAL, 23));
        panes.push_back(separator()); panes.push_back(conversation);
        if (ftxui::Terminal::Size().dimx >= 110) {
            panes.push_back(separator());
            panes.push_back(vbox({text(" ONLINE") | bold, text(" Enter: private chat") | dim, separator(),
                member_menu_->Render() | vscroll_indicator | frame | flex}) | size(WIDTH, EQUAL, 22));
        }
        return hbox(std::move(panes)) | flex;
    });
    file_menu_ = Menu(&file_labels_, &selected_file_);
    auto accept = Button("Accept", [this] { accept_file(false); }, style);
    auto save_as = Button("Save as", [this] { accept_file(true); }, style);
    auto decline = Button("Decline", [this] { cancel_file(true); }, style);
    auto cancel = Button("Cancel transfer", [this] { cancel_file(false); }, style);
    auto file_actions = Container::Horizontal({accept, save_as, decline, cancel});
    auto files = Renderer(Container::Vertical({file_menu_, file_actions}), [this, file_actions] {
        return vbox({text(" FILE TRANSFERS") | bold | color(accent),
            text(" Offers require consent from each recipient. SHA-256 is checked before saving.") | dim,
            separator(), file_menu_->Render() | vscroll_indicator | frame | flex, separator(),
            transfer_details(), separator(), file_actions->Render()}) | flex;
    });
    auto join_room = [this] {
        if (selected_room_ >= 0 && selected_room_ < static_cast<int>(model_.rooms.size()))
            command({{"type", "room_join"}, {"name", model_.rooms[static_cast<std::size_t>(selected_room_)].second}});
    };
    auto rooms_option = MenuOption::Vertical(); rooms_option.on_enter = join_room;
    room_menu_ = Menu(&room_labels_, &selected_room_, rooms_option);
    InputOption one_line; one_line.multiline = false;
    auto room_input = Input(&room_name_, "New room name", one_line);
    auto create = Button("Create room", [this] {
        command({{"type", "room_create"}, {"name", trim(room_name_)}}); room_name_.clear();
    }, style);
    auto join = Button("Join selected", join_room, style);
    auto room_actions = Container::Horizontal({room_input, create, join});
    auto rooms = Renderer(Container::Vertical({room_menu_, room_actions}), [this, room_actions] {
        return vbox({text(" ROOMS") | bold | color(accent),
            paragraph(" Public rooms are visible to everyone on this server. Only joined members receive their messages and file offers."),
            separator(), room_menu_->Render() | vscroll_indicator | frame | flex, separator(), room_actions->Render()}) | flex;
    });
    auto reconnect = Button("Reconnect", [this] { command({{"type", "reconnect"}}); }, style);
    auto quit = Button("Quit", [this] { screen_.Exit(); }, style);
    auto help_actions = Container::Horizontal({reconnect, quit});
    auto help = Renderer(help_actions, [this, help_actions] {
        Elements content{
            text(" LEMONCHAT / QUICK REFERENCE") | bold | color(accent), separator(),
            text(" Tab / Shift+Tab    Move focus      Ctrl+L    Composer"),
            text(" F1 Help   F2 Rooms   F3 Files       Ctrl+F    Search"),
            text(" Enter send   Ctrl+N newline         Ctrl+Q    Quit"), separator(),
            text(" /room NAME    Create and join a room"), text(" /join NAME    Join an existing room"),
            text(" /dm NAME      Open a private conversation with an online peer"),
            text(" /leave        Leave the active room"), text(" /send PATH    Offer a file to this conversation"),
            text(" /accept ID [PATH]   /decline ID   /cancel ID"),
            text(" /search TEXT  Search local history (PgUp/PgDn to scroll)"),
            text(" /save PATH    Export this conversation as plaintext JSON"),
            text(" /reconnect    Connect again after a lost connection"), separator(),
            paragraph(" TLS 1.3 encrypts every connection. The host is trusted and can read relayed content. Invitations grant access: share them privately. Message 'sent' means the server accepted it, not that anyone read it."),
            paragraph(" History is held in memory (2,000 messages / 8 MiB). Leaving the app clears it. Files are saved only after acceptance and verification.")};
        if (application_ && !application_->invitation().empty()) {
            content.push_back(separator()); content.push_back(text(" YOUR SERVER INVITATION") | bold);
            content.push_back(paragraph(application_->invitation()) | color(accent));
        }
        content.push_back(filler()); content.push_back(help_actions->Render());
        return vbox(std::move(content)) | flex;
    });
    auto tabs = Menu(&tabs_, &tab_, MenuOption::HorizontalAnimated());
    auto pages = Container::Tab({chat, files, rooms, help}, &tab_);
    normal_ = Renderer(Container::Vertical({tabs, pages}), [this, tabs, pages] {
        return vbox({hbox({text(" LEMONCHAT ") | bold | color(accent), text("/ " + model_.name) | dim,
                filler(), text(model_.connected ? "● connected " : "○ offline ") | color(model_.connected ? accent : Color::Red)}),
            tabs->Render(), separator(), pages->Render() | flex, separator(),
            text(" " + terminal_text(model_.status)) | size(HEIGHT, EQUAL, 1),
            text(" Tab navigate · F1 help · F2 rooms · F3 files · Ctrl+Q quit") | dim}) | border;
    });
    auto apply_dialog = [this] {
        try {
            const auto value = trim(dialog_value_);
            if (value.empty()) throw std::runtime_error("A path is required");
            dialog_action_(value); stage_ = 0;
            if (tab_ == 1) file_menu_->TakeFocus();
            else input_->TakeFocus();
        } catch (const std::exception& error) { dialog_error_ = error.what(); }
    };
    auto dialog_option = one_line; dialog_option.on_enter = apply_dialog;
    dialog_input_ = Input(&dialog_value_, "File path", dialog_option);
    auto confirm = Button("Continue", apply_dialog, style);
    auto dismiss = Button("Back", [this] { stage_ = 0; input_->TakeFocus(); }, style);
    auto dialog_actions = Container::Horizontal({confirm, dismiss});
    dialog_ = Renderer(Container::Vertical({dialog_input_, dialog_actions}), [this, dialog_actions] {
        return vbox({text(dialog_title_) | bold | color(accent), separator(), paragraph(dialog_text_),
            dialog_input_->Render() | border, text(dialog_error_) | color(Color::Red),
            dialog_actions->Render()}) | size(WIDTH, LESS_THAN, 76) | border | clear_under | center;
    });
    auto mode = Menu(&modes_, &launch_mode_, MenuOption::Vertical());
    auto name = Input(&launch_name_, "Nickname", one_line);
    auto invitation_option = one_line;
    invitation_option.on_enter = [this] { start(true); };
    auto invitation = Input(&launch_invite_, "lemonchat://…", invitation_option);
    auto port = Input(&launch_port_, "4567", one_line);
    auto bind = Input(&launch_bind_, "0.0.0.0", one_line);
    auto advertise = Input(&launch_advertise_, "Host/IP reachable by other participants", one_line);
    auto hosting = Renderer(Container::Vertical({port, bind, advertise}), [port, bind, advertise] {
        return vbox({hbox({text(" Port       "), port->Render() | flex}),
            hbox({text(" Listen on  "), bind->Render() | flex}),
            hbox({text(" Share host "), advertise->Render() | flex})});
    });
    auto joining = Renderer(invitation, [invitation] { return vbox({text(" Invitation"), invitation->Render() | border}); });
    auto connection = Container::Tab({hosting, joining}, &launch_mode_);
    auto launch_button = Button("Start", [this] { start(true); }, style);
    auto launch_quit = Button("Quit", [this] { screen_.Exit(); }, style);
    auto launch_actions = Container::Horizontal({launch_button, launch_quit});
    launch_ = Renderer(Container::Vertical({name, mode, connection, launch_actions}),
        [this, name, mode, connection, launch_actions] {
        return vbox({text(" LEMONCHAT") | bold | color(accent), text(" Conversations, without a browser.") | dim,
            separator(), hbox({text(" Nickname   "), name->Render() | flex}), separator(), mode->Render(), separator(),
            connection->Render(), separator(),
            paragraph(" TLS 1.3 • Verify invitations through a trusted channel. The server host can read relayed content."),
            paragraph(" For another computer to join, Share host must be a reachable IP or hostname; allow the listening port through your firewall.") | dim,
            text(terminal_text(launch_error_)) | color(Color::Red), launch_actions->Render()}) |
            size(WIDTH, LESS_THAN, 80) | border | center;
    });
    root_ = Renderer(Container::Tab({normal_, dialog_, launch_}, &stage_), [this] {
        if (stage_ == 2) return launch_->Render();
        if (stage_ == 1) return dbox({normal_->Render() | dim, dialog_->Render()});
        return normal_->Render();
    });
    const Components chat_focus{tabs, chat_menu_, find_, input_, send, attach, leave, member_menu_};
    const Components files_focus{tabs, file_menu_, accept, save_as, decline, cancel};
    const Components rooms_focus{tabs, room_menu_, room_input, create, join};
    const Components help_focus{tabs, reconnect, quit};
    const Components host_focus{name, mode, port, bind, advertise, launch_button, launch_quit};
    const Components join_focus{name, mode, invitation, launch_button, launch_quit};
    const Components dialog_focus{dialog_input_, confirm, dismiss};
    root_ = CatchEvent(root_, [this, chat_focus, files_focus, rooms_focus, help_focus, host_focus, join_focus, dialog_focus](Event event) {
        if (event == Event::Custom) { drain(); return true; }
        if (event == Event::CtrlQ) { screen_.Exit(); return true; }
        if (event == Event::Tab || event == Event::TabReverse) {
            // Explicit focus order: nested FTXUI menus otherwise consume Tab internally.
            const auto& order = stage_ == 2 ? (launch_mode_ == 0 ? host_focus : join_focus) :
                (stage_ == 1 ? dialog_focus : (tab_ == 0 ? chat_focus : (tab_ == 1 ? files_focus : (tab_ == 2 ? rooms_focus : help_focus))));
            const int count = static_cast<int>(order.size());
            int current = -1;
            for (int i = 0; i < count; ++i) if (order[static_cast<std::size_t>(i)]->Focused()) current = i;
            for (int step = 1; step <= count; ++step) {
                const auto index = static_cast<std::size_t>((current + (event == Event::Tab ? step : -step) + 2 * count) % count);
                const auto& target = order[index];
                if (stage_ == 0 && tab_ == 0 &&
                    ((target == chat_menu_ && ftxui::Terminal::Size().dimx < 80) ||
                     (target == member_menu_ && ftxui::Terminal::Size().dimx < 110))) continue;
                if (target->Focusable()) { target->TakeFocus(); break; }
            }
            return true;
        }
        if (event == Event::Escape && stage_ == 1) { stage_ = 0; input_->TakeFocus(); return true; }
        if (stage_ != 0) return false;
        if (event == Event::F1) { tab_ = 3; help_focus[1]->TakeFocus(); return true; }
        if (event == Event::F2) { tab_ = 2; room_menu_->TakeFocus(); return true; }
        if (event == Event::F3) { tab_ = 1; file_menu_->TakeFocus(); return true; }
        if (event == Event::CtrlF) { tab_ = 0; find_->TakeFocus(); return true; }
        if (event == Event::CtrlL) { tab_ = 0; input_->TakeFocus(); return true; }
        if (tab_ == 0 && event == Event::PageUp) { scroll_ += 5; return true; }
        if (tab_ == 0 && event == Event::PageDown) { scroll_ = std::max(0, scroll_ - 5); return true; }
        if (tab_ == 0 && input_->Focused() && event == Event::CtrlN) {
            composer_.insert(static_cast<std::size_t>(std::clamp(composer_cursor_, 0, static_cast<int>(composer_.size()))), "\n");
            ++composer_cursor_; return true;
        }
        return false;
    });
}
} // namespace

int run_tui(AppOptions options, bool launch_form) {
    if (options.client.downloads.empty()) options.client.downloads = options.data_directory / "downloads";
    return Terminal(std::move(options), launch_form).run();
}

} // namespace lemon
