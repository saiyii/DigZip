// Whole-archive fuzzing: format detection, every container reader, every codec, path rules.
// Only digzip::Error may come out of the engine; anything else (crash, std::bad_alloc,
// std::out_of_range, sanitizer report) is a bug.

#include "extract/path_rules.hpp"
#include "format/reader.hpp"
#include "io/stream.hpp"
#include "monitor.hpp"

#include <digzip/archive.hpp>

#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace digzip;

    Options options;
    options.limits.max_total_size = 64u << 20;
    options.limits.max_memory = 64u << 20;
    options.limits.max_entries = 10'000;
    options.passwords = {"secret"};

    const io::SourcePtr source = io::memory_source(std::vector<std::uint8_t>(data, data + size));
    std::vector<std::uint8_t> buffer(1 << 16);
    try {
        Monitor monitor(options, source->size());
        format::OpenedArchive opened = format::open_archive(source, "fuzz.bin", monitor);
        const extraction::NameRules rules{true, 256};
        Entry entry;
        while (opened.reader->next(entry)) {
            monitor.add_entry(entry.path);
            try {
                const extraction::SafePath path = extraction::sanitize_path(entry.path, rules);
                if (entry.type == EntryType::Symlink) {
                    extraction::resolve_link_target(path.components, entry.link_target);
                }
            } catch (const Error&) {
            }
            if (entry.type != EntryType::File) {
                continue;
            }
            try {
                io::InputStream& stream = opened.reader->data();
                while (stream.read(buffer.data(), buffer.size()) > 0) {
                }
            } catch (const Error& e) {
                if (e.code() == ErrorCode::LimitExceeded || !opened.reader->independent_entries()) {
                    break;
                }
            }
        }
    } catch (const Error&) {
    }
    return 0;
}
