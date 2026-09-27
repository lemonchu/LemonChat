#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

namespace lemon {
// An exclusive same-directory temporary file, published only on explicit commit.
class IncomingFile {
public:
    explicit IncomingFile(std::filesystem::path destination);
    ~IncomingFile();
    IncomingFile(const IncomingFile&) = delete;
    IncomingFile& operator=(const IncomingFile&) = delete;
    void write(const void* data, std::size_t size);
    void commit();
    const std::filesystem::path& path() const { return destination_; }
private:
    std::filesystem::path destination_, temporary_;
    std::FILE* file_ = nullptr;
    bool committed_ = false;
};
void private_directory(const std::filesystem::path& path);
void write_new_file(const std::filesystem::path& path, const std::string& content);
std::filesystem::path default_data_directory();
} // namespace lemon
