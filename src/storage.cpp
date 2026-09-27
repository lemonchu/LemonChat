#include "lemonchat/storage.hpp"
#include "lemonchat/crypto.hpp"
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lemon {
namespace fs = std::filesystem;
namespace {
std::runtime_error file_error(const std::string& action) {
    return std::runtime_error(action + ": " + std::generic_category().message(errno));
}
}
IncomingFile::IncomingFile(fs::path destination) : destination_(std::move(destination)) {
    if (destination_.filename().empty()) throw std::runtime_error("Choose an output file name");
    if (fs::exists(fs::symlink_status(destination_))) throw std::runtime_error("Destination already exists");
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary_ = destination_.parent_path() / (".lemonchat-" + random_hex() + ".part");
#ifdef _WIN32
        int fd = -1;
        const int code = _wsopen_s(&fd, temporary_.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                                   _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (code) errno = code;
#else
        const int fd = ::open(temporary_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd < 0) {
            if (errno == EEXIST) continue;
            throw file_error("Create temporary file");
        }
#ifdef _WIN32
        file_ = _fdopen(fd, "wb");
#else
        file_ = fdopen(fd, "wb");
#endif
        if (file_) return;
        const auto error = file_error("Open file stream");
#ifdef _WIN32
        _close(fd);
#else
        ::close(fd);
#endif
        std::error_code ignored;
        fs::remove(temporary_, ignored);
        throw error;
    }
    throw std::runtime_error("Could not allocate a temporary file");
}
IncomingFile::~IncomingFile() {
    if (file_) std::fclose(file_);
    std::error_code ignored;
    fs::remove(temporary_, ignored);
}
void IncomingFile::write(const void* data, std::size_t size) {
    if (!file_ || std::fwrite(data, 1, size, file_) != size) throw file_error("Write file");
}
void IncomingFile::commit() {
    if (!file_ || committed_) throw std::runtime_error("File is already closed");
    if (std::fflush(file_) != 0) throw file_error("Flush file");
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) throw file_error("Sync file");
#else
    if (fsync(fileno(file_)) != 0) throw file_error("Sync file");
#endif
    const int code = std::fclose(file_);
    file_ = nullptr;
    if (code != 0) throw file_error("Close file");
#ifdef _WIN32
    if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Publish file: " + std::system_category().message(static_cast<int>(GetLastError())));
#else
    if (::link(temporary_.c_str(), destination_.c_str()) != 0) throw file_error("Publish file without overwriting");
#endif
    committed_ = true;
}
void private_directory(const fs::path& path) {
    fs::create_directories(path);
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
}
void write_new_file(const fs::path& path, const std::string& content) {
    IncomingFile file(path);
    file.write(content.data(), content.size());
    file.commit();
}
fs::path default_data_directory() {
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"LOCALAPPDATA")) return fs::path(value) / L"LemonChat";
#else
    if (const auto* value = std::getenv("XDG_DATA_HOME")) return fs::path(value) / "lemonchat";
    if (const auto* value = std::getenv("HOME")) return fs::path(value) / ".local/share/lemonchat";
#endif
    return fs::current_path() / ".lemonchat";
}
} // namespace lemon
