#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace gmb::io {
// Shared by the Scene Bundle/report writer and the native FileGDB writer.
// These checks reject existing link components; they are not a sandbox against
// a process replacing parent directories while a conversion is in progress.
void reject_reparse(const std::filesystem::path& path);

// Lexical containment after making paths absolute; includes equality. Windows
// uses ordinal case-insensitive component comparison and rejects ambiguous
// trailing dots/spaces in components; Linux is case-sensitive.
// Link rejection is a separate requirement before using either path.
bool within(const std::filesystem::path& child, const std::filesystem::path& root);

// Creates exclusively and flushes the complete contents. A failed create never
// removes the existing entry. A failed write cleans up only the file it created.
void write_exclusive(const std::filesystem::path& path, const std::string& data);

// Streams arbitrarily large contents through the same exclusive-create handle.
// The producer must call the sink synchronously and must not retain it. Producer
// or write failures remove only the file created by this invocation.
using WriteChunk = std::function<void(const char*, std::size_t)>;
void write_exclusive(const std::filesystem::path& path,
                     const std::function<void(const WriteChunk&)>& produce);

// Commit a staged file/directory on the same filesystem. An existing target,
// including an empty directory or dangling link, must never be replaced. Fail
// closed if the filesystem cannot provide a no-replace rename.
bool move_new(const std::filesystem::path& source, const std::filesystem::path& target);
}
