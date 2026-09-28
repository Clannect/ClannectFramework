// cfw_embed_resources: files built into the binary come back byte for byte
// by their paths, and nothing else is found.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "cfw/test/Check.h"
#include "testResource.h"

using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::vector<char> readFile(const char *relative) {
    std::ifstream in(std::string(CFW_EMBED_SOURCE_DIR) + "/" + relative, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool same(cfw::Span<const std::byte> embedded, const std::vector<char> &file) {
    if (embedded.size() != file.size()) {
        return false;
    }
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (embedded[i] != std::byte(static_cast<unsigned char>(file[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const auto note = cfwtest::testResource("embedded/note.txt");
    checkEqual(note.size(), std::size_t{25}, "the text file's size");
    check(same(note, readFile("embedded/note.txt")), "and its bytes");
    const auto random = cfwtest::testResource("embedded/random.bin");
    check(same(random, readFile("embedded/random.bin")), "binary data comes back unchanged");
    check(cfwtest::testResource("embedded/missing.txt").empty(), "an unknown path is empty");
    check(cfwtest::testResource("note.txt").empty(), "paths are relative to the base");
    return cfw::test::finish("ResourceEmbedTest");
}
