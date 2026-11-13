// Stand-alone driver for the fuzz targets when libFuzzer is not available (MSVC, MinGW, GCC).
// Usage: fuzz_<name> [--mutations N] [--seed S] <file or directory>...
// Runs every input, then N mutated copies of each (bit flips, overwritten bytes, truncation,
// duplicated slices). A crash or sanitizer report is a finding; the mutation is printed first.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> data, std::mt19937_64& random)
{
    if (data.empty()) {
        data.push_back(0);
    }
    const auto pick = [&](std::size_t n) { return static_cast<std::size_t>(random() % n); };
    const int edits = 1 + static_cast<int>(random() % 4);
    for (int e = 0; e < edits && !data.empty(); ++e) {
        switch (random() % 4) {
        case 0:
            data[pick(data.size())] ^= static_cast<std::uint8_t>(1u << (random() % 8));
            break;
        case 1:
            data[pick(data.size())] = static_cast<std::uint8_t>(random());
            break;
        case 2:
            data.resize(pick(data.size()) + 1);
            break;
        default: {
            const std::size_t from = pick(data.size());
            const std::size_t length = std::min<std::size_t>(data.size() - from, 1 + pick(64));
            const std::vector<std::uint8_t> slice(data.begin() + static_cast<std::ptrdiff_t>(from),
                                                  data.begin() + static_cast<std::ptrdiff_t>(from + length));
            data.insert(data.begin() + static_cast<std::ptrdiff_t>(pick(data.size())), slice.begin(), slice.end());
            break;
        }
        }
    }
    return data;
}

} // namespace

int main(int argc, char** argv)
{
    unsigned long mutations = 0;
    std::uint64_t seed = 1;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mutations" && i + 1 < argc) {
            mutations = std::strtoul(argv[++i], nullptr, 10);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (fs::is_directory(arg)) {
            for (const auto& entry : fs::recursive_directory_iterator(arg)) {
                if (entry.is_regular_file()) {
                    inputs.push_back(entry.path());
                }
            }
        } else {
            inputs.emplace_back(arg);
        }
    }

    std::mt19937_64 random(seed);
    std::size_t runs = 0;
    for (const fs::path& input : inputs) {
        const std::vector<std::uint8_t> data = read_file(input);
        LLVMFuzzerTestOneInput(data.data(), data.size());
        ++runs;
        for (unsigned long m = 0; m < mutations; ++m) {
            const std::uint64_t state = random();
            std::mt19937_64 local(state);
            const std::vector<std::uint8_t> mutated = mutate(data, local);
            std::fprintf(stderr, "\r%s mutation %lu (seed %llu)   ", input.filename().string().c_str(), m,
                         static_cast<unsigned long long>(state));
            LLVMFuzzerTestOneInput(mutated.data(), mutated.size());
            ++runs;
        }
    }
    std::fprintf(stderr, "\n%zu inputs, %zu runs, no crash\n", inputs.size(), runs);
    return 0;
}
