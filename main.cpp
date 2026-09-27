#include "lemonchat/tui.hpp"
#include "lemonchat/storage.hpp"
#include <clocale>
#include <csignal>
#include <iostream>
#include <mutex>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace lemon;
void usage() {
    std::cout << R"(LemonChat 2.0.0 — encrypted terminal conversations and file transfers

Usage:
  lemonchat                              Interactive launch form
  lemonchat host [options]                Host a server and open the TUI
  lemonchat join --invite LINK [options]   Join a server in the TUI
  lemonchat serve [options]               Run a dedicated server

Options:
  --name NAME          Nickname (default: lemon)
  --invite LINK        Private invitation from the host
  --port PORT          Listening port (default: 4567; 0 selects a free port)
  --bind ADDRESS       Listening IP (default: 0.0.0.0)
  --advertise HOST     Reachable hostname/IP for invitations (default: 127.0.0.1)
  --data-dir PATH      Private identity and default downloads location
  --downloads PATH     Directory for accepted files
  --max-file BYTES     Maximum accepted file size (default: 1073741824)
  --max-clients COUNT  Server connection limit (default: 64; maximum: 1024)
  --headless           JSON-lines commands/events, using the same client core
  --ready-file PATH    Write dedicated-server invitation JSON to a new private file
  --help              Show this help
  --version           Print version

Connections require TLS 1.3, a pinned certificate, and the invitation secret.
The host is trusted and can read relayed content. This is not end-to-end encryption.
No automatic port forwarding: the host must be reachable by every participant.
In the TUI press F1 for help. In headless mode send {"type":"quit"} or close stdin.
)";
}
std::uint64_t number(const std::string& text, std::uint64_t maximum) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected an unsigned decimal integer: " + text);
    const auto value = std::stoull(text);
    if (value > maximum) throw std::runtime_error("Value exceeds allowed range: " + text);
    return value;
}
bool terminal_available() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}
int run(const std::vector<std::string>& arguments) {
    std::setlocale(LC_ALL, "");
    AppOptions options;
    options.data_directory = default_data_directory();
    std::string mode;
    bool headless = false;
    std::filesystem::path ready_file;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        auto value = [&]() -> const std::string& {
            if (++i == arguments.size()) throw std::runtime_error("Missing value after " + arg);
            return arguments[i];
        };
        if (arg == "--help" || arg == "-h") { usage(); return 0; }
        if (arg == "--version") { std::cout << "LemonChat 2.0.0\n"; return 0; }
        if (arg == "--render-demo") {
            const auto width = number(value(), 300);
            const auto height = number(value(), 100);
            if (width < 60 || height < 15) throw std::runtime_error("Demo size must be at least 60 x 15");
            std::cout << render_demo(static_cast<int>(width), static_cast<int>(height)) << '\n'; return 0;
        }
        if (arg == "host" || arg == "join" || arg == "serve") {
            if (!mode.empty()) throw std::runtime_error("Choose only one mode");
            mode = arg;
        } else if (arg == "--name") options.client.name = value();
        else if (arg == "--invite") options.client.invitation = Invitation::parse(value());
        else if (arg == "--port") options.server.port = static_cast<std::uint16_t>(number(value(), 65535));
        else if (arg == "--bind") options.server.bind = value();
        else if (arg == "--advertise") options.server.advertise = value();
        else if (arg == "--data-dir") options.data_directory = std::filesystem::u8path(value());
        else if (arg == "--downloads") options.client.downloads = std::filesystem::u8path(value());
        else if (arg == "--max-file") options.server.max_file = options.client.max_file = number(value(), UINT64_MAX);
        else if (arg == "--max-clients") {
            options.server.max_clients = static_cast<std::size_t>(number(value(), 1024));
            if (!options.server.max_clients) throw std::runtime_error("At least one client must be allowed");
        } else if (arg == "--headless") headless = true;
        else if (arg == "--ready-file") ready_file = std::filesystem::u8path(value());
        else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (mode == "join" && !options.client.invitation.port) throw std::runtime_error("join requires --invite LINK");
    if (mode != "serve" && !ready_file.empty()) throw std::runtime_error("--ready-file requires serve");
    if (mode.empty() && headless) throw std::runtime_error("--headless requires host or join");
    if (options.data_directory.empty()) throw std::runtime_error("Data directory must not be empty");
    if (mode == "serve") {
        asio::io_context io;
        Server server(io, load_identity(options.data_directory / "host"), options.server);
        const auto invitation = server.invitation().str();
        Invitation::parse(invitation);
        Json ready{{"type", "ready"}, {"invitation", invitation}, {"port", server.invitation().port}};
        if (!ready_file.empty()) write_new_file(ready_file, ready.dump() + "\n");
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const asio::error_code&, int) { server.stop(); });
        server.start();
        std::cout << ready.dump() << std::endl;
        io.run(); return 0;
    }
    options.host = mode == "host";
    if (!headless) {
        if (!terminal_available()) throw std::runtime_error("The TUI requires a terminal; use --headless for JSON-lines automation");
        return run_tui(std::move(options), mode.empty());
    }
    std::mutex output_mutex;
    auto emit = [&](Json event) {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << event.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
    };
    Application application(std::move(options), emit);
    if (!application.invitation().empty()) emit({{"type", "ready"}, {"invitation", application.invitation()}});
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            if (line.size() > max_frame) throw std::runtime_error("Command exceeds 128 KiB");
            auto command = Json::parse(line);
            if (!command.is_object()) throw std::runtime_error("Expected a JSON object");
            if (command.value("type", "") == "quit") break;
            application.command(std::move(command));
        } catch (const std::exception& error) { emit({{"type", "error"}, {"message", error.what()}}); }
    }
    application.stop();
    return 0;
}
int entry(const std::vector<std::string>& arguments) {
    try { return run(arguments); }
    catch (const std::exception& error) { std::cerr << "lemonchat: " << error.what() << '\n'; return 1; }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) { std::cerr << "Invalid Unicode argument\n"; return 1; }
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), size, nullptr, nullptr);
        value.pop_back(); arguments.push_back(std::move(value));
    }
    return entry(arguments);
}
#else
int main(int argc, char* argv[]) { return entry(std::vector<std::string>(argv, argv + argc)); }
#endif
