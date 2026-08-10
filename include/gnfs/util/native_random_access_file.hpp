#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace gnfs::util {

/// Move-only positioned binary I/O over one native file.
///
/// Exact operations either transfer the complete span or throw. Range errors
/// are rejected before I/O. Once native I/O begins, an exception may leave a
/// read destination or write range partially modified: discard a failed read
/// buffer and treat a failed write range as indeterminate. The class owns no
/// shared cursor and deliberately does not provide pathname identity,
/// publication, or parent-directory durability guarantees. Callers must not
/// race I/O with move, close, or sync on the same object.
class NativeRandomAccessFile final {
public:
    NativeRandomAccessFile() noexcept;
    ~NativeRandomAccessFile();

    NativeRandomAccessFile(const NativeRandomAccessFile&) = delete;
    NativeRandomAccessFile& operator=(const NativeRandomAccessFile&) = delete;

    NativeRandomAccessFile(NativeRandomAccessFile&&) noexcept;
    NativeRandomAccessFile& operator=(NativeRandomAccessFile&&) noexcept;

    [[nodiscard]] static NativeRandomAccessFile create_truncated(const std::filesystem::path& path);
    [[nodiscard]] static NativeRandomAccessFile open_read_only(const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size() const;

    void read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const;
    void write_exact_at(std::uint64_t offset, std::span<const std::byte> source);

    /// Flush this file's data and metadata. This is not a parent-directory
    /// durability or atomic-publication boundary.
    void sync();

    /// Release the native handle without throwing. Call sync() first when the
    /// caller needs a checked file durability boundary.
    void close() noexcept;

private:
    struct State;

    explicit NativeRandomAccessFile(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

} // namespace gnfs::util
