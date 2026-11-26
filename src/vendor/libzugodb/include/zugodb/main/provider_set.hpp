#pragma once

// The engine's side of the host-supplied providers (Stage 11.1, #40;
// design L25 and L29). providers.hpp is the C interface a host fills in;
// this is what the engine keeps of it, and the wrappers through which the
// binder and the executor call it, turning every error status into an
// exception.

#include "zugodb/main/providers.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zugodb {

enum class ProviderService : unsigned int { REGEX = 0, UTF8 = 1, UUID = 2, RANDOM = 3, DECIMAL = 4 };

// What became of a service's table. Only ACTIVE is used; every other state
// leaves the service's functions refused as when no table is given.
enum class ProviderState : unsigned int {
    ABSENT,        // no table (NULL), or SetProviders was not called
    ACTIVE,        // checked and in use
    WRONG_VERSION, // abi_version is not this engine's
    TOO_SMALL,     // struct_size is below this engine's table
    NULL_MEMBER,   // a member of this engine's table is NULL
    RESERVED,      // a slot for a later service: never read
};

const char *ProviderStateName(ProviderState state);

// The tables of one Database. Set checks each table and copies the
// members it uses, so the host's structs need not outlive the call (the
// functions they point to must).
class ProviderSet {
public:
    // Throws InvalidInputException for a NULL set, or one whose own
    // abi_version or struct_size is not this engine's: those are a host's
    // bug. A service table that is unusable is recorded as such, not thrown.
    void Set(const zugodb_providers *providers);

    ProviderState State(ProviderService service) const;

    // NULL unless the service is ACTIVE.
    const zugodb_regex_v1 *Regex() const;
    const zugodb_utf8_v1 *Utf8() const;

private:
    zugodb_regex_v1 regex_{};
    zugodb_utf8_v1 utf8_{};
    ProviderState states_[5] = {ProviderState::ABSENT, ProviderState::ABSENT,
                                ProviderState::ABSENT, ProviderState::ABSENT,
                                ProviderState::ABSENT};
};

// The text of a provider's status: its strerror, else a generic one.
std::string ProviderStatusText(const char *(*strerror_fn)(int), int status);

// ---------------------------------------------------------------------------
// Regular expressions.
// ---------------------------------------------------------------------------

// A compiled pattern, read-only once made, shared by every thread of a
// query. `what` names the function in errors.
class ProviderRegex {
public:
    // Throws InvalidInputException when the provider refuses the pattern
    // or returns any other error.
    static std::shared_ptr<const ProviderRegex> Compile(const zugodb_regex_v1 &api,
                                                        const std::string &what,
                                                        const std::string &pattern,
                                                        uint32_t flags);
    ~ProviderRegex();
    ProviderRegex(const ProviderRegex &) = delete;
    ProviderRegex &operator=(const ProviderRegex &) = delete;

    const zugodb_regex_v1 &Api() const { return *api_; }
    zugodb_regex_pattern *Pattern() const { return pattern_; }
    int GroupCount() const { return groups_; }
    const std::string &Text() const { return text_; }
    uint32_t Flags() const { return flags_; }
    const std::string &What() const { return what_; }

private:
    ProviderRegex() = default;
    const zugodb_regex_v1 *api_ = nullptr;
    zugodb_regex_pattern *pattern_ = nullptr;
    int groups_ = 0;
    std::string text_;
    uint32_t flags_ = 0;
    std::string what_;
};

// Match state for one pattern, on one thread, for one call; freed by its
// destructor, so on a throw too.
class ProviderRegexMatch {
public:
    explicit ProviderRegexMatch(const ProviderRegex &re);
    ~ProviderRegexMatch();
    ProviderRegexMatch(const ProviderRegexMatch &) = delete;
    ProviderRegexMatch &operator=(const ProviderRegexMatch &) = delete;

    // Whether the pattern matches (the whole text, with `whole`).
    bool Search(const char *text, size_t len, bool whole);
    // Group `g` of the last match: false when it took no part.
    bool Group(int g, size_t &start, size_t &end);
    // The text with the first, or every, match replaced by `rewrite`.
    std::string Replace(const char *text, size_t len, const std::string &rewrite, bool global);

private:
    [[noreturn]] void Fail(int status) const;
    const ProviderRegex &re_;
    zugodb_regex_match *match_ = nullptr;
};

// One regex function call as the binder resolved it, held by its
// BoundFunction and shared by every thread that runs it.
struct ProviderRegexCall {
    enum class Kind { MATCHES, FULL_MATCH, REPLACE, EXTRACT };
    Kind kind = Kind::MATCHES;
    const zugodb_regex_v1 *api = nullptr; // the Database's table
    std::string what;                     // the function's name, for errors
    uint32_t flags = 0;                   // ZUGODB_REGEX_* compile flags
    bool global = false;                  // regexp_replace's 'g'
    int group = 0;                        // regexp_extract's group
    // A constant pattern, compiled once at bind; NULL for a pattern that
    // is computed per row, or a NULL constant (constant_null).
    std::shared_ptr<const ProviderRegex> constant;
    bool constant_null = false;
};

// ---------------------------------------------------------------------------
// UTF-8.
// ---------------------------------------------------------------------------

// The code points of `s`: the provider's count when `api` is not NULL and
// it finds `s` valid; the engine's (bytes that do not continue a sequence)
// otherwise, the same answer. A provider error other than invalid input
// throws.
size_t CountCodePoints(const zugodb_utf8_v1 *api, const char *s, size_t n);

} // namespace zugodb
