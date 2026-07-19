#include <nxgallery/release_update.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>

namespace nxgallery {
namespace {

struct SemanticVersion {
    std::array<unsigned long, 3> component{};
};

bool parse_version(const std::string &text, SemanticVersion &version) noexcept {
    const char *cursor = text.c_str();
    if (*cursor == 'v') ++cursor;
    for (std::size_t index = 0; index < version.component.size(); ++index) {
        if (*cursor < '0' || *cursor > '9') return false;
        errno = 0;
        char *end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 10);
        if (errno == ERANGE || end == cursor) return false;
        version.component[index] = value;
        cursor = end;
        if (index + 1 < version.component.size()) {
            if (*cursor != '.') return false;
            ++cursor;
        }
    }
    return *cursor == '\0';
}

}  // namespace

bool is_newer_release(const std::string &current,
                      const std::string &candidate) noexcept {
    SemanticVersion current_version;
    SemanticVersion candidate_version;
    if (!parse_version(current, current_version) ||
        !parse_version(candidate, candidate_version)) {
        return false;
    }
    return candidate_version.component > current_version.component;
}

}  // namespace nxgallery
