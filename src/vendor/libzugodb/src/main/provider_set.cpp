#include "zugodb/main/provider_set.hpp"

#include "zugodb/common/exception.hpp"

#include <cstdint>
#include <limits>

namespace zugodb {

const char *ProviderStateName(ProviderState state) {
    switch (state) {
    case ProviderState::ABSENT: return "absent";
    case ProviderState::ACTIVE: return "active";
    case ProviderState::WRONG_VERSION: return "wrong ABI version";
    case ProviderState::TOO_SMALL: return "struct_size too small";
    case ProviderState::NULL_MEMBER: return "a NULL member";
    case ProviderState::RESERVED: return "reserved";
    }
    return "unknown";
}

// The state of a table whose members the engine copies: its header, then
// every member of this engine's version present.
template <class Table>
static ProviderState CheckTable(const Table *t, uint32_t version) {
    if (!t) return ProviderState::ABSENT;
    if (t->abi_version != version) return ProviderState::WRONG_VERSION;
    if (t->struct_size < sizeof(Table)) return ProviderState::TOO_SMALL;
    return ProviderState::ACTIVE;
}

void ProviderSet::Set(const zugodb_providers *p) {
    if (!p) throw InvalidInputException("SetProviders: the provider set is NULL");
    if (p->abi_version != ZUGODB_PROVIDERS_ABI_VERSION) {
        throw InvalidInputException("SetProviders: the provider set has ABI version " +
                                    std::to_string(p->abi_version) + ", not " +
                                    std::to_string(ZUGODB_PROVIDERS_ABI_VERSION));
    }
    if (p->struct_size < sizeof(zugodb_providers)) {
        throw InvalidInputException("SetProviders: the provider set's struct_size is " +
                                    std::to_string(p->struct_size) + ", below " +
                                    std::to_string(sizeof(zugodb_providers)));
    }

    auto rs = CheckTable(p->regex, ZUGODB_REGEX_ABI_VERSION);
    if (rs == ProviderState::ACTIVE) {
        const auto &r = *p->regex;
        if (!r.compile || !r.pattern_free || !r.group_count || !r.match_create ||
            !r.match_free || !r.search || !r.group || !r.replace || !r.strerror) {
            rs = ProviderState::NULL_MEMBER;
        } else {
            regex_ = r;
        }
    }
    states_[static_cast<unsigned>(ProviderService::REGEX)] = rs;

    auto us = CheckTable(p->utf8, ZUGODB_UTF8_ABI_VERSION);
    if (us == ProviderState::ACTIVE) {
        const auto &u = *p->utf8;
        if (!u.validate || !u.count || !u.strerror) {
            us = ProviderState::NULL_MEMBER;
        } else {
            utf8_ = u;
        }
    }
    states_[static_cast<unsigned>(ProviderService::UTF8)] = us;

    states_[static_cast<unsigned>(ProviderService::UUID)] =
        p->uuid ? ProviderState::RESERVED : ProviderState::ABSENT;
    states_[static_cast<unsigned>(ProviderService::RANDOM)] =
        p->random ? ProviderState::RESERVED : ProviderState::ABSENT;
    states_[static_cast<unsigned>(ProviderService::DECIMAL)] =
        p->decimal ? ProviderState::RESERVED : ProviderState::ABSENT;
}

ProviderState ProviderSet::State(ProviderService service) const {
    auto i = static_cast<unsigned>(service);
    return i < 5 ? states_[i] : ProviderState::ABSENT;
}

const zugodb_regex_v1 *ProviderSet::Regex() const {
    return State(ProviderService::REGEX) == ProviderState::ACTIVE ? &regex_ : nullptr;
}

const zugodb_utf8_v1 *ProviderSet::Utf8() const {
    return State(ProviderService::UTF8) == ProviderState::ACTIVE ? &utf8_ : nullptr;
}

std::string ProviderStatusText(const char *(*strerror_fn)(int), int status) {
    const char *msg = strerror_fn ? strerror_fn(status) : nullptr;
    std::string text = (msg && *msg) ? std::string(msg) : std::string("an error");
    return text + " (provider status " + std::to_string(status) + ")";
}

// The error of a provider's status. A limit hit says so: it is never
// taken as no match.
[[noreturn]] static void ThrowStatus(const std::string &what, const char *(*strerror_fn)(int),
                                     int status) {
    std::string prefix = what + ": ";
    if (status == ZUGODB_PROVIDER_ERR_LIMIT) prefix += "the regex provider hit a limit: ";
    throw InvalidInputException(prefix + ProviderStatusText(strerror_fn, status));
}

std::shared_ptr<const ProviderRegex> ProviderRegex::Compile(const zugodb_regex_v1 &api,
                                                            const std::string &what,
                                                            const std::string &pattern,
                                                            uint32_t flags) {
    std::shared_ptr<ProviderRegex> re(new ProviderRegex());
    re->api_ = &api;
    re->text_ = pattern;
    re->flags_ = flags;
    re->what_ = what;
    char err[256] = {0};
    zugodb_regex_pattern *p = nullptr;
    int st = api.compile(pattern.data(), pattern.size(), flags, &p, err, sizeof err);
    if (st != ZUGODB_PROVIDER_OK || !p) {
        if (p) api.pattern_free(p);
        err[sizeof err - 1] = '\0';
        std::string msg = what + ": the pattern is refused: " +
                          ProviderStatusText(api.strerror, st == ZUGODB_PROVIDER_OK
                                                               ? ZUGODB_PROVIDER_ERR_OTHER
                                                               : st);
        if (err[0]) msg += ": " + std::string(err);
        throw InvalidInputException(msg);
    }
    re->pattern_ = p;
    int groups = api.group_count(p);
    if (groups < 0) ThrowStatus(what, api.strerror, groups);
    re->groups_ = groups;
    return re;
}

ProviderRegex::~ProviderRegex() {
    if (pattern_) api_->pattern_free(pattern_);
}

ProviderRegexMatch::ProviderRegexMatch(const ProviderRegex &re) : re_(re) {
    int st = re.Api().match_create(re.Pattern(), &match_);
    if (st != ZUGODB_PROVIDER_OK || !match_) {
        if (match_) re.Api().match_free(match_);
        match_ = nullptr;
        Fail(st == ZUGODB_PROVIDER_OK ? ZUGODB_PROVIDER_ERR_OTHER : st);
    }
}

ProviderRegexMatch::~ProviderRegexMatch() {
    if (match_) re_.Api().match_free(match_);
}

void ProviderRegexMatch::Fail(int status) const {
    ThrowStatus(re_.What(), re_.Api().strerror, status);
}

bool ProviderRegexMatch::Search(const char *text, size_t len, bool whole) {
    int st = re_.Api().search(re_.Pattern(), text, len, whole ? ZUGODB_REGEX_ANCHOR_BOTH : 0u,
                              match_);
    if (st == ZUGODB_PROVIDER_OK) return true;
    if (st == ZUGODB_PROVIDER_NOMATCH) return false;
    Fail(st);
}

bool ProviderRegexMatch::Group(int g, size_t &start, size_t &end) {
    int st = re_.Api().group(match_, g, &start, &end);
    if (st == ZUGODB_PROVIDER_NOMATCH) return false;
    if (st != ZUGODB_PROVIDER_OK) Fail(st);
    if (start > end) Fail(ZUGODB_PROVIDER_ERR_OTHER);
    return true;
}

std::string ProviderRegexMatch::Replace(const char *text, size_t len, const std::string &rewrite,
                                        bool global) {
    // A VARCHAR holds at most 4 GiB - 1 bytes (string_t's size is 32 bits).
    constexpr size_t kMax = std::numeric_limits<uint32_t>::max();
    uint32_t opts = global ? ZUGODB_REGEX_REPLACE_GLOBAL : 0u;
    size_t cap = len + rewrite.size() + 64;
    std::string out;
    for (int attempt = 0; attempt < 2; attempt++) {
        out.resize(cap);
        size_t outlen = cap;
        int st = re_.Api().replace(re_.Pattern(), text, len, rewrite.data(), rewrite.size(),
                                   opts, match_, out.empty() ? nullptr : &out[0], &outlen);
        if (st == ZUGODB_PROVIDER_OK) {
            if (outlen > cap) Fail(ZUGODB_PROVIDER_ERR_OTHER);
            out.resize(outlen);
            return out;
        }
        if (st != ZUGODB_PROVIDER_ERR_BUFFER_TOO_SMALL) Fail(st);
        // Retried once, at the size the provider asks for, if it is more.
        if (outlen <= cap) Fail(st);
        if (outlen > kMax) {
            throw InvalidInputException(re_.What() + ": the result is longer than a VARCHAR holds");
        }
        cap = outlen;
    }
    Fail(ZUGODB_PROVIDER_ERR_BUFFER_TOO_SMALL);
}

static size_t BuiltinCodePoints(const char *s, size_t n) {
    size_t cp = 0;
    for (size_t j = 0; j < n; j++) {
        if ((static_cast<unsigned char>(s[j]) & 0xC0) != 0x80) cp++;
    }
    return cp;
}

size_t CountCodePoints(const zugodb_utf8_v1 *api, const char *s, size_t n) {
    if (!api) return BuiltinCodePoints(s, n);
    int st = api->validate(s, n);
    if (st == ZUGODB_PROVIDER_ERR_INVALID_UTF8) return BuiltinCodePoints(s, n);
    if (st != ZUGODB_PROVIDER_OK) {
        throw InvalidInputException("the UTF-8 provider: " +
                                    ProviderStatusText(api->strerror, st));
    }
    size_t out = 0;
    st = api->count(s, n, &out);
    if (st != ZUGODB_PROVIDER_OK) {
        throw InvalidInputException("the UTF-8 provider: " +
                                    ProviderStatusText(api->strerror, st));
    }
    // A count above the bytes is no count of UTF-8.
    if (out > n) {
        throw InvalidInputException("the UTF-8 provider counted " + std::to_string(out) +
                                    " code points in " + std::to_string(n) + " bytes");
    }
    return out;
}

} // namespace zugodb
