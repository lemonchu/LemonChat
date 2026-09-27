#include "lemonchat/crypto.hpp"
#include "lemonchat/model.hpp"
#include "lemonchat/storage.hpp"
#include "lemonchat/transport.hpp"
#include <fstream>
#include <iostream>

using namespace lemon;
namespace fs = std::filesystem;
void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "Invalid operation was accepted");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
    const auto directory = fs::temp_directory_path() / ("lemonchat-core-" + random_hex());
    try {
        Sha256 hash; hash.update("a", 1); hash.update("bc", 2);
        check(hash.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
        rejects([&] { hash.finish(); });
        check(constant_equal("secret", "secret") && !constant_equal("secret", "Secret"), "Secret comparison");
        const auto identity = load_identity(directory);
        check(load_identity(directory).pin == identity.pin, "Identity persists across restarts");
        check(identity.secret.size() == 64 && identity.pin.size() == 64, "Identity sizes");
        for (const std::string host : {"localhost", "127.0.0.1", "::1"}) {
            const Invitation invitation{host, 4567, identity.pin, identity.secret};
            const auto parsed = Invitation::parse(invitation.str());
            check(parsed.host == host && parsed.pin == identity.pin && parsed.secret == identity.secret && parsed.port == 4567,
                  "Invitation round trip");
        }
        for (const std::string bad : {"", "http://localhost:4567", "lemonchat://localhost:0?pin=x&key=y",
                "lemonchat://user@localhost:4567?pin=x&key=y"}) rejects([&] { Invitation::parse(bad); });
        std::vector<unsigned char> binary(chunk_size);
        for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<unsigned char>(i);
        const auto id = random_hex();
        const auto packet = chunk_packet(id, 0x1020304050607080ULL, binary.data(), binary.size());
        const auto chunk = parse_chunk(packet);
        check(chunk.id == id && chunk.offset == 0x1020304050607080ULL && chunk.size == binary.size(), "Binary framing fields");
        check(std::equal(binary.begin(), binary.end(), chunk.data), "Binary framing data");
        rejects([&] { chunk_packet(id, 0, binary.data(), 0); });
        rejects([&] { parse_chunk(json_packet({{"type", "ping"}})); });
        rejects([&] { json_packet({{"text", std::string(max_frame, 'x')}}); });
        const auto destination = directory / fs::u8path("文件 with spaces.bin");
        {
            IncomingFile file(destination); file.write(binary.data(), binary.size());
            check(!fs::exists(destination), "Partial file must not be published");
        }
        check(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1, "Aborted temporary cleaned up");
        {
            IncomingFile file(destination); file.write("original", 8); file.commit();
        }
        check(read(destination) == "original", "Atomic file content");
        rejects([&] { IncomingFile file(destination); });
        const auto race = directory / "race.bin";
        {
            IncomingFile file(race); file.write("incoming", 8);
            write_new_file(race, "created during transfer");
            rejects([&] { file.commit(); });
        }
        check(read(race) == "created during transfer", "Destination race preserves original");
        check(!safe_filename("../escape") && !safe_filename("a:b") && safe_filename("中文.txt"), "Portable safe names");
        check(terminal_text("\x1b[31mhi\r\a") == "?[31mhi??", "Terminal control bytes stripped");
        check(terminal_text("\xc2\x9b" "31m中文") == "?31m中文", "UTF-8 C1 controls stripped");
        Model model;
        model.apply({{"type", "welcome"}, {"self", "me"}, {"name", "lemon"}});
        model.apply({{"type", "message_pending"}, {"conversation", "lobby"}, {"request", "request"},
            {"sender", "me"}, {"name", "lemon"}, {"text", "你好 World"}, {"time", 1}});
        model.apply({{"type", "message"}, {"conversation", "lobby"}, {"id", "actual"}, {"request", "request"},
            {"sender", "me"}, {"name", "lemon"}, {"text", "你好 World"}, {"time", 2}});
        check(model.conversations.at("lobby").messages.size() == 1 && model.conversations.at("lobby").messages.front().status == "sent", "Echo replaces pending message");
        check(model.search("lobby", "world").size() == 1 && model.search("lobby", "你好").size() == 1, "History search");
        for (int i = 0; i < 2100; ++i) model.apply({{"type", "message"}, {"conversation", "lobby"},
            {"sender", "other"}, {"text", "history"}, {"time", i + 3}});
        check(model.conversations.at("lobby").messages.size() == 2000, "History is bounded");
        model.apply({{"type", "message"}, {"conversation", "room"}, {"sender", "other"}, {"text", "unread"}});
        check(model.conversations.at("room").unread == 1, "Unread conversation count");
        model.select("room"); check(model.conversations.at("room").unread == 0, "Selecting clears unread");
        model.apply({{"type", "disconnected"}, {"reason", "test"}});
        check(!model.connected, "Disconnected state");
        fs::remove_all(directory);
        std::cout << "Core checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored; fs::remove_all(directory, ignored);
        return 1;
    }
}
