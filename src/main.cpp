  #include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

struct TokenSpan {
    std::size_t begin{};
    std::size_t end{};
    std::string raw;
    std::string norm;
};

struct Document {
    std::uint32_t id{};
    std::string path;
    std::string title;
    std::string type;
    std::uint32_t length{};
    std::uint32_t first_fragment_id{};
    std::uint32_t fragment_count{};
    std::uint32_t segment_id{};
};

struct Fragment {
    std::uint32_t id{};
    std::uint32_t doc_id{};
    std::uint32_t ordinal{};
    std::string text;
    std::uint32_t length{};
    bool metadata_only{};
};

struct Posting {
    std::uint32_t fragment_id{};
    std::uint32_t tf{};
};

struct DocPosting {
    std::uint32_t doc_id{};
    std::uint32_t tf{};
};

struct Segment {
    std::uint32_t id{};
    std::uint32_t first_doc_id{};
    std::uint32_t doc_count{};
    std::uint32_t first_fragment_id{};
    std::uint32_t fragment_count{};
    std::uint32_t length{};
};

struct SearchResult {
    std::uint32_t fragment_id{};
    double score{};
};

std::string pathToUtf8(const fs::path& path) {
#if defined(__cpp_char8_t)
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
}

std::string wideToUtf8(const std::wstring& wide) {
#ifdef _WIN32
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

std::wstring utf8ToWide(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), static_cast<int>(utf8.size()),
                                   nullptr, 0);
    }
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), size);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

fs::path pathFromUtf8(const std::string& value) {
#ifdef _WIN32
    return fs::path(utf8ToWide(value));
#else
    return fs::path(value);
#endif
}

template <typename T>
void writePod(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool readPod(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

void writeSizedString(std::ostream& out, const std::string& value) {
    writePod<std::uint64_t>(out, static_cast<std::uint64_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool readSizedString(std::istream& in, std::string& value) {
    std::uint64_t size = 0;
    if (!readPod<std::uint64_t>(in, size)) {
        return false;
    }
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    value.assign(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        in.read(value.data(), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(in);
}

std::uint64_t stableHash64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') {
            ch = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(std::string text) {
    const char* ws = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(ws);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool isContinuationByte(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

bool decodeNextUtf8(const std::string& text, std::size_t& i, std::uint32_t& cp) {
    if (i >= text.size()) {
        return false;
    }

    const unsigned char c0 = static_cast<unsigned char>(text[i++]);
    if (c0 < 0x80U) {
        cp = c0;
        return true;
    }

    int extra = 0;
    std::uint32_t value = 0;
    if ((c0 & 0xE0U) == 0xC0U) {
        extra = 1;
        value = c0 & 0x1FU;
    } else if ((c0 & 0xF0U) == 0xE0U) {
        extra = 2;
        value = c0 & 0x0FU;
    } else if ((c0 & 0xF8U) == 0xF0U) {
        extra = 3;
        value = c0 & 0x07U;
    } else {
        cp = 0xFFFDU;
        return true;
    }

    for (int k = 0; k < extra; ++k) {
        if (i >= text.size()) {
            cp = 0xFFFDU;
            return true;
        }
        const unsigned char cx = static_cast<unsigned char>(text[i]);
        if (!isContinuationByte(cx)) {
            cp = 0xFFFDU;
            return true;
        }
        ++i;
        value = (value << 6U) | (cx & 0x3FU);
    }

    cp = value;
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::vector<std::uint32_t> utf8ToCodepoints(const std::string& text) {
    std::vector<std::uint32_t> cps;
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        cps.push_back(cp);
    }
    return cps;
}

std::size_t codepointCount(const std::string& text) {
    std::size_t count = 0;
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        ++count;
    }
    return count;
}

std::uint32_t lowerCodepoint(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp - 'A' + 'a';
    }
    if (cp == 0x0401U || cp == 0x0451U) {
        return 0x0435U;
    }
    if (cp >= 0x0410U && cp <= 0x042FU) {
        return cp + 32U;
    }
    return cp;
}

std::string lowerUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        appendUtf8(out, lowerCodepoint(cp));
    }
    return out;
}

bool isWordCodepoint(std::uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) {
        return true;
    }
    if (cp >= 0x0400U && cp <= 0x04FFU) {
        return true;
    }
    if (cp >= 0x00C0U && cp <= 0x024FU) {
        return true;
    }
    return false;
}

const std::unordered_set<std::string>& stopWords() {
    static const std::unordered_set<std::string> words = {
        "a", "an", "and", "are", "as", "at", "be", "by", "for", "from", "in", "is",
        "it", "of", "on", "or", "that", "the", "to", "was", "were", "with",
        "без", "бы", "был", "была", "были", "было", "быть", "в", "вам", "вас",
        "весь", "во", "вот", "все", "всего", "всех", "вы", "где", "да", "для",
        "до", "его", "ее", "если", "есть", "еще", "же", "за", "и", "из", "или",
        "им", "их", "к", "как", "ко", "когда", "кто", "ли", "либо", "мне", "может",
        "мы", "на", "над", "надо", "наш", "не", "него", "нее", "нет", "ни", "них",
        "но", "о", "об", "однако", "он", "она", "они", "оно", "от", "по", "под",
        "при", "про", "с", "со", "так", "также", "там", "те", "тем", "то", "того",
        "тоже", "той", "только", "том", "ты", "у", "уже", "что", "чтобы", "это",
        "этого", "этой", "этом", "этот", "я"
    };
    return words;
}

std::string removeSuffix(const std::string& term, const std::vector<std::string>& suffixes) {
    const std::size_t term_len = codepointCount(term);
    for (const std::string& suffix : suffixes) {
        const std::size_t suffix_len = codepointCount(suffix);
        if (term_len >= suffix_len + 3 && endsWith(term, suffix)) {
            return term.substr(0, term.size() - suffix.size());
        }
    }
    return term;
}

std::string normalizeTerm(const std::string& raw) {
    std::string term = lowerUtf8(raw);
    if (term.empty() || stopWords().count(term) != 0) {
        return {};
    }

    static const std::vector<std::string> ru_suffixes = {
        "иями", "ями", "ами", "иях", "ах", "ях", "ого", "его", "ому", "ему",
        "ыми", "ими", "ешь", "ете", "ает", "ают", "яет", "яют", "ить", "ится",
        "ать", "ться", "ее", "ие", "ые", "ое", "ая", "яя", "ый", "ий", "ой",
        "ам", "ям", "ов", "ев", "ей", "ия", "ья", "ью", "ии", "ть", "ти",
        "ся", "ет", "ют", "ут", "ит", "ат", "ят", "ем", "им", "ым", "ом",
        "ую", "юю", "а", "я", "ы", "и", "о", "е", "у", "ю", "ь"
    };
    static const std::vector<std::string> en_suffixes = {
        "ization", "ational", "fulness", "ousness", "iveness", "tional", "ing",
        "edly", "edly", "ment", "ness", "able", "ible", "ions", "tion", "sion",
        "ies", "ied", "ed", "ly", "es", "s"
    };

    term = removeSuffix(term, ru_suffixes);
    term = removeSuffix(term, en_suffixes);

    if (term.size() <= 1 || stopWords().count(term) != 0) {
        return {};
    }
    return term;
}

std::vector<TokenSpan> tokenizeWithSpans(const std::string& text) {
    std::vector<TokenSpan> tokens;
    std::size_t i = 0;
    std::size_t token_begin = std::string::npos;
    std::size_t token_end = 0;
    std::string raw;
    std::uint32_t cp = 0;

    auto finish = [&]() {
        if (token_begin == std::string::npos) {
            return;
        }
        const std::string norm = normalizeTerm(raw);
        if (!norm.empty()) {
            tokens.push_back(TokenSpan{token_begin, token_end, raw, norm});
        }
        token_begin = std::string::npos;
        raw.clear();
    };

    while (i < text.size()) {
        const std::size_t cp_begin = i;
        if (!decodeNextUtf8(text, i, cp)) {
            break;
        }
        const std::size_t cp_end = i;
        if (isWordCodepoint(cp)) {
            if (token_begin == std::string::npos) {
                token_begin = cp_begin;
            }
            token_end = cp_end;
            appendUtf8(raw, lowerCodepoint(cp));
        } else {
            finish();
        }
    }
    finish();
    return tokens;
}

std::vector<std::string> queryBaseTerms(const std::string& query) {
    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;
    for (const TokenSpan& token : tokenizeWithSpans(query)) {
        if (seen.insert(token.norm).second) {
            terms.push_back(token.norm);
        }
    }
    return terms;
}

bool isValidUtf8(const std::string& bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c0 = static_cast<unsigned char>(bytes[i++]);
        if (c0 < 0x80U) {
            continue;
        }
        int extra = 0;
        if ((c0 & 0xE0U) == 0xC0U) {
            extra = 1;
        } else if ((c0 & 0xF0U) == 0xE0U) {
            extra = 2;
        } else if ((c0 & 0xF8U) == 0xF0U) {
            extra = 3;
        } else {
            return false;
        }
        for (int k = 0; k < extra; ++k) {
            if (i >= bytes.size() || !isContinuationByte(static_cast<unsigned char>(bytes[i++]))) {
                return false;
            }
        }
    }
    return true;
}

std::uint32_t cp1251ToCodepoint(unsigned char byte) {
    if (byte < 0x80U) {
        return byte;
    }
    if (byte >= 0xC0U) {
        return 0x0410U + (byte - 0xC0U);
    }
    switch (byte) {
        case 0x82U: return 0x201AU;
        case 0x84U: return 0x201EU;
        case 0x85U: return 0x2026U;
        case 0x86U: return 0x2020U;
        case 0x87U: return 0x2021U;
        case 0x88U: return 0x20ACU;
        case 0x89U: return 0x2030U;
        case 0x91U: return 0x2018U;
        case 0x92U: return 0x2019U;
        case 0x93U: return 0x201CU;
        case 0x94U: return 0x201DU;
        case 0x95U: return 0x2022U;
        case 0x96U: return 0x2013U;
        case 0x97U: return 0x2014U;
        case 0x99U: return 0x2122U;
        case 0x9AU: return 0x0459U;
        case 0x9CU: return 0x045AU;
        case 0x9EU: return 0x045EU;
        case 0x9FU: return 0x045FU;
        case 0xA0U: return 0x00A0U;
        case 0xA7U: return 0x00A7U;
        case 0xA8U: return 0x0401U;
        case 0xABU: return 0x00ABU;
        case 0xADU: return 0x00ADU;
        case 0xB9U: return 0x2116U;
        case 0xB8U: return 0x0451U;
        case 0xBBU: return 0x00BBU;
        case 0xA5U: return 0x0490U;
        case 0xB4U: return 0x0491U;
        case 0xAAU: return 0x0404U;
        case 0xBAU: return 0x0454U;
        case 0xAFU: return 0x0407U;
        case 0xBFU: return 0x0457U;
        case 0xB2U: return 0x0406U;
        case 0xB3U: return 0x0456U;
        default: return '?';
    }
}

std::string cp1251ToUtf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char byte : bytes) {
        appendUtf8(out, cp1251ToCodepoint(byte));
    }
    return out;
}

std::string utf16ToUtf8(const std::string& bytes, bool little_endian, std::size_t offset) {
    std::string out;
    for (std::size_t i = offset; i + 1 < bytes.size();) {
        auto read_unit = [&](std::size_t pos) -> std::uint16_t {
            const unsigned char b0 = static_cast<unsigned char>(bytes[pos]);
            const unsigned char b1 = static_cast<unsigned char>(bytes[pos + 1]);
            return little_endian ? static_cast<std::uint16_t>(b0 | (b1 << 8U))
                                 : static_cast<std::uint16_t>((b0 << 8U) | b1);
        };
        std::uint32_t cp = read_unit(i);
        i += 2;
        if (cp >= 0xD800U && cp <= 0xDBFFU && i + 1 < bytes.size()) {
            const std::uint32_t low = read_unit(i);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                i += 2;
                cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
            }
        }
        appendUtf8(out, cp);
    }
    return out;
}

std::string bytesToUtf8Text(const std::string& bytes) {
    if (startsWith(bytes, "\xEF\xBB\xBF")) {
        return bytes.substr(3);
    }
    if (startsWith(bytes, "\xFF\xFE")) {
        return utf16ToUtf8(bytes, true, 2);
    }
    if (startsWith(bytes, "\xFE\xFF")) {
        return utf16ToUtf8(bytes, false, 2);
    }
    if (isValidUtf8(bytes)) {
        return bytes;
    }
    return cp1251ToUtf8(bytes);
}

bool isCyrillicCodepoint(std::uint32_t cp) {
    return (cp >= 0x0400U && cp <= 0x052FU) ||
           (cp >= 0x2DE0U && cp <= 0x2DFFU) ||
           (cp >= 0xA640U && cp <= 0xA69FU);
}

bool isMojibakeCp1251Codepoint(std::uint32_t cp) {
    return (cp >= 0x00C0U && cp <= 0x00FFU) ||
           (cp >= 0x0080U && cp <= 0x009FU) ||
           cp == 0x001CU || cp == 0x001DU;
}

struct TextQuality {
    std::size_t cyrillic{};
    std::size_t latin1_suspect{};
    std::size_t word_chars{};
    std::size_t controls{};
};

TextQuality inspectTextQuality(const std::string& text) {
    TextQuality quality;
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        if (isCyrillicCodepoint(cp)) {
            ++quality.cyrillic;
        }
        if (isMojibakeCp1251Codepoint(cp)) {
            ++quality.latin1_suspect;
        }
        if (isWordCodepoint(cp)) {
            ++quality.word_chars;
        }
        if ((cp < 0x20U && cp != '\n' && cp != '\r' && cp != '\t' && cp != '\f') ||
            (cp >= 0x80U && cp <= 0x9FU)) {
            ++quality.controls;
        }
    }
    return quality;
}

std::string cp1251MojibakeToUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        if (cp == 0x001CU) {
            appendUtf8(out, 0x0420U); // Common broken PDF mapping for capital Er.
        } else if (cp == 0x001DU) {
            appendUtf8(out, 0x0444U); // Common broken PDF mapping for ef.
        } else if (cp == 0x009FU) {
            appendUtf8(out, 0x00A7U); // Often appears instead of a section sign in old PDFs.
        } else if (cp == '\f') {
            out.push_back('\n');
        } else if (cp <= 0x7FU) {
            if (cp < 0x20U && cp != '\n' && cp != '\r' && cp != '\t') {
                out.push_back(' ');
            } else {
                out.push_back(static_cast<char>(cp));
            }
        } else if (cp <= 0x00FFU) {
            appendUtf8(out, cp1251ToCodepoint(static_cast<unsigned char>(cp)));
        } else {
            appendUtf8(out, cp);
        }
    }
    return out;
}

std::string stripProblemControls(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    std::uint32_t cp = 0;
    while (decodeNextUtf8(text, i, cp)) {
        if (cp == '\f') {
            out.push_back('\n');
        } else if (cp < 0x20U && cp != '\n' && cp != '\r' && cp != '\t') {
            out.push_back(' ');
        } else if (cp >= 0x80U && cp <= 0x9FU) {
            out.push_back(' ');
        } else {
            appendUtf8(out, cp);
        }
    }
    return out;
}

std::string repairCp1251Mojibake(std::string text) {
    const TextQuality before = inspectTextQuality(text);
    if (before.latin1_suspect < 8 || before.latin1_suspect <= before.cyrillic * 2) {
        return stripProblemControls(std::move(text));
    }

    std::string repaired = cp1251MojibakeToUtf8(text);
    const TextQuality after = inspectTextQuality(repaired);
    if (after.cyrillic > before.cyrillic + before.latin1_suspect / 2) {
        return stripProblemControls(std::move(repaired));
    }
    return stripProblemControls(std::move(text));
}

bool hasUsefulExtractedText(const std::string& text) {
    const auto tokens = tokenizeWithSpans(text);
    if (tokens.size() < 3) {
        return false;
    }
    const TextQuality quality = inspectTextQuality(text);
    if (quality.word_chars < 12) {
        return false;
    }
    return quality.controls <= quality.word_chars / 3 + 20;
}

std::string cleanPdfExtractedText(std::string text) {
    text = repairCp1251Mojibake(std::move(text));
    replaceAll(text, "\r\n", "\n");
    replaceAll(text, "\r", "\n");
    if (!hasUsefulExtractedText(text)) {
        return {};
    }
    return text;
}

std::string readBinaryPrefix(const fs::path& path, std::uint64_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in || max_bytes == 0) {
        return {};
    }

    std::string out;
    out.resize(static_cast<std::size_t>(std::min<std::uint64_t>(max_bytes, 1024ULL * 1024ULL)));
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return out;
}

std::size_t countSubstring(std::string_view text, std::string_view needle) {
    if (needle.empty() || text.empty() || needle.size() > text.size()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

struct PdfProbe {
    std::uint64_t size{};
    std::size_t image_markers{};
    std::size_t font_markers{};
    std::size_t text_markers{};
    std::size_t page_markers{};
    bool likely_scanned{};
    bool likely_textual{};
    std::uint32_t pdftotext_timeout_ms{10000};
    std::uint32_t mutool_timeout_ms{7000};
    std::uint64_t max_output_bytes{16ULL * 1024ULL * 1024ULL};
    bool try_mutool{true};
    bool try_fallback{false};
};

PdfProbe inspectPdf(const fs::path& path) {
    PdfProbe probe;
    std::error_code ec;
    probe.size = static_cast<std::uint64_t>(fs::file_size(path, ec));
    if (ec) {
        probe.size = 0;
    }

    const std::uint64_t sample_bytes = probe.size <= 2ULL * 1024ULL * 1024ULL
        ? probe.size
        : 768ULL * 1024ULL;
    const std::string sample = readBinaryPrefix(path, sample_bytes);

    probe.image_markers = countSubstring(sample, "/Subtype /Image") + countSubstring(sample, "/Image");
    probe.font_markers = countSubstring(sample, "/Font");
    probe.text_markers =
        countSubstring(sample, " BT") +
        countSubstring(sample, "\nBT") +
        countSubstring(sample, " Tj") +
        countSubstring(sample, " TJ") +
        countSubstring(sample, "'") / 4;
    probe.page_markers = countSubstring(sample, "/Type /Page");

    probe.likely_textual = probe.font_markers > 0 || probe.text_markers > 0;
    probe.likely_scanned =
        probe.size >= 4ULL * 1024ULL * 1024ULL &&
        probe.image_markers >= 4 &&
        probe.text_markers == 0 &&
        probe.font_markers == 0;

    if (probe.size >= 64ULL * 1024ULL * 1024ULL) {
        probe.pdftotext_timeout_ms = 3500;
        probe.mutool_timeout_ms = 2500;
        probe.max_output_bytes = 8ULL * 1024ULL * 1024ULL;
    } else if (probe.size >= 16ULL * 1024ULL * 1024ULL) {
        probe.pdftotext_timeout_ms = 5000;
        probe.mutool_timeout_ms = 3500;
        probe.max_output_bytes = 10ULL * 1024ULL * 1024ULL;
    } else if (probe.size >= 8ULL * 1024ULL * 1024ULL) {
        probe.pdftotext_timeout_ms = 7000;
        probe.mutool_timeout_ms = 4500;
        probe.max_output_bytes = 12ULL * 1024ULL * 1024ULL;
    }

    probe.try_mutool = !probe.likely_scanned &&
        (probe.likely_textual || probe.size <= 6ULL * 1024ULL * 1024ULL);

    probe.try_fallback =
        probe.size <= 2ULL * 1024ULL * 1024ULL &&
        (probe.likely_textual || probe.text_markers > 0);

    return probe;
}

std::optional<std::string> readBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string normalizeNewlines(std::string text) {
    replaceAll(text, "\r\n", "\n");
    replaceAll(text, "\r", "\n");
    return text;
}

std::string stripMarkdown(std::string text) {
    text = normalizeNewlines(std::move(text));
    text = std::regex_replace(text, std::regex(R"(!\[([^\]]*)\]\([^\)]*\))"), " ");
    text = std::regex_replace(text, std::regex(R"(\[([^\]]+)\]\([^\)]*\))"), "$1");
    text = std::regex_replace(text, std::regex(R"(`+)"), "");
    text = std::regex_replace(text, std::regex(R"(\*\*|__|\*|_)"), "");

    std::ostringstream out;
    std::istringstream in(text);
    std::string line;
    bool in_code_block = false;
    while (std::getline(in, line)) {
        std::string stripped = trim(line);
        if (startsWith(stripped, "```") || startsWith(stripped, "~~~")) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) {
            continue;
        }
        std::size_t pos = 0;
        while (pos < line.size() && line[pos] == '#') {
            ++pos;
        }
        if (pos > 0 && pos < line.size() && line[pos] == ' ') {
            line = line.substr(pos + 1);
        }
        stripped = trim(line);
        if (startsWith(stripped, "- ") || startsWith(stripped, "* ") || startsWith(stripped, "+ ")) {
            line = stripped.substr(2);
        }
        if (stripped.size() > 2 && std::isdigit(static_cast<unsigned char>(stripped[0])) &&
            stripped[1] == '.' && stripped[2] == ' ') {
            line = stripped.substr(3);
        }
        out << line << '\n';
    }
    return out.str();
}

#ifndef _WIN32
std::string shellDoubleQuote(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

std::string powerShellSingleQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string escapeForPowerShellCommand(const std::string& script) {
    std::string out;
    out.reserve(script.size());
    for (char ch : script) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string runCommandCapture(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return output;
    }

    std::array<char, 4096> buffer{};
    while (true) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (n > 0) {
            output.append(buffer.data(), n);
        }
        if (n < buffer.size()) {
            if (std::feof(pipe) != 0 || std::ferror(pipe) != 0) {
                break;
            }
        }
    }

    pclose(pipe);
    return bytesToUtf8Text(output);
}
#endif

#ifdef _WIN32
std::wstring quoteWindowsArg(const std::wstring& arg) {
    const bool needs_quotes = arg.empty() ||
        arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        return arg;
    }

    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(ch);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring joinWindowsCommandLine(const std::vector<std::wstring>& args) {
    std::wstring command;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command.push_back(L' ');
        }
        command += quoteWindowsArg(args[i]);
    }
    return command;
}

std::wstring makeTempOutputPath() {
    std::array<wchar_t, MAX_PATH + 1> temp_dir{};
    const DWORD dir_len = GetTempPathW(static_cast<DWORD>(temp_dir.size()), temp_dir.data());
    if (dir_len == 0 || dir_len >= temp_dir.size()) {
        return {};
    }

    std::array<wchar_t, MAX_PATH + 1> temp_file{};
    if (GetTempFileNameW(temp_dir.data(), L"dcs", 0, temp_file.data()) == 0) {
        return {};
    }
    return temp_file.data();
}

std::string readFilePrefixW(const std::wstring& path, std::uint64_t max_bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const std::uint64_t bytes_to_read = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(std::max<LONGLONG>(0, size.QuadPart)),
        max_bytes
    );

    std::string output;
    output.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(bytes_to_read, 1024U * 1024U)));

    std::array<char, 8192> buffer{};
    std::uint64_t total = 0;
    while (total < bytes_to_read) {
        const DWORD want = static_cast<DWORD>(
            std::min<std::uint64_t>(buffer.size(), bytes_to_read - total)
        );
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), want, &got, nullptr) || got == 0) {
            break;
        }
        output.append(buffer.data(), got);
        total += got;
    }

    CloseHandle(file);
    return output;
}

std::string runProcessCaptureW(const std::vector<std::wstring>& args,
                               DWORD timeout_ms = 30000,
                               std::uint64_t max_output_bytes = 64ULL * 1024ULL * 1024ULL) {
    if (args.empty()) {
        return {};
    }

    SECURITY_ATTRIBUTES inherit_sa{};
    inherit_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    inherit_sa.bInheritHandle = TRUE;

    const std::wstring output_path = makeTempOutputPath();
    if (output_path.empty()) {
        return {};
    }
    HANDLE output_file = CreateFileW(output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     &inherit_sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output_file == INVALID_HANDLE_VALUE) {
        DeleteFileW(output_path.c_str());
        return {};
    }

    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inherit_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inherit_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul_in != INVALID_HANDLE_VALUE ? nul_in : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_file;
    startup.hStdError = nul_err != INVALID_HANDLE_VALUE ? nul_err : output_file;

    PROCESS_INFORMATION process{};
    std::wstring command_line = joinWindowsCommandLine(args);
    const BOOL created = CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process
    );

    if (nul_in != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_in);
    }
    if (nul_err != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_err);
    }

    if (!created) {
        CloseHandle(output_file);
        DeleteFileW(output_path.c_str());
        return {};
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, timeout_ms);
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        timed_out = true;
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(output_file);

    if (timed_out) {
        DeleteFileW(output_path.c_str());
        return {};
    }

    std::string output = readFilePrefixW(output_path, max_output_bytes);
    DeleteFileW(output_path.c_str());
    return bytesToUtf8Text(output);
}

std::wstring powerShellSingleQuoteW(const std::wstring& value) {
    std::wstring out = L"'";
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            out += L"''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(L'\'');
    return out;
}
#endif

std::string decodeXmlEntities(std::string text) {
    replaceAll(text, "&lt;", "<");
    replaceAll(text, "&gt;", ">");
    replaceAll(text, "&quot;", "\"");
    replaceAll(text, "&apos;", "'");
    replaceAll(text, "&amp;", "&");
    return text;
}

std::string docxXmlToText(std::string xml) {
    if (xml.empty()) {
        return {};
    }
    xml = std::regex_replace(xml, std::regex(R"(<w:tab[^>]*/>)"), "\t");
    xml = std::regex_replace(xml, std::regex(R"(<w:br[^>]*/>)"), "\n");
    xml = std::regex_replace(xml, std::regex(R"(</w:tc>)"), "\t");
    xml = std::regex_replace(xml, std::regex(R"(</w:p>)"), "\n\n");
    xml = std::regex_replace(xml, std::regex(R"(<[^>]+>)"), "");
    return normalizeNewlines(decodeXmlEntities(xml));
}

std::string extractDocxText(const fs::path& path) {
#ifdef _WIN32
    const std::wstring p = path.wstring();
    const std::wstring script =
        L"$ErrorActionPreference='Stop';"
        L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        L"Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        L"$zip=[IO.Compression.ZipFile]::OpenRead(" + powerShellSingleQuoteW(p) + L");"
        L"try {"
        L"foreach ($e in $zip.Entries) {"
        L"if ($e.FullName -match '^word/(document|footnotes|endnotes|comments|header[0-9]+|footer[0-9]+)\\.xml$') {"
        L"$stream=$e.Open();"
        L"$reader=[IO.StreamReader]::new($stream);"
        L"$content=$reader.ReadToEnd();"
        L"$reader.Close();"
        L"$stream.Close();"
        L"[Console]::WriteLine($content);"
        L"}"
        L"}"
        L"} finally { $zip.Dispose(); }";
    return docxXmlToText(runProcessCaptureW({
        L"powershell.exe",
        L"-NoProfile",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-Command",
        script
    }));
#else
    const std::string p = pathToUtf8(path);
    const std::string script =
        "$ErrorActionPreference='Stop';"
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        "Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        "$zip=[IO.Compression.ZipFile]::OpenRead(" + powerShellSingleQuote(p) + ");"
        "try {"
        "foreach ($e in $zip.Entries) {"
        "if ($e.FullName -match '^word/(document|footnotes|endnotes|comments|header[0-9]+|footer[0-9]+)\\.xml$') {"
        "$stream=$e.Open();"
        "$reader=[IO.StreamReader]::new($stream);"
        "$content=$reader.ReadToEnd();"
        "$reader.Close();"
        "$stream.Close();"
        "[Console]::WriteLine($content);"
        "}"
        "}"
        "} finally { $zip.Dispose(); }";
    const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -Command \"" +
                                escapeForPowerShellCommand(script) + "\"";
    return docxXmlToText(runCommandCapture(command));
#endif
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string pdfBytesToText(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFEU &&
        static_cast<unsigned char>(bytes[1]) == 0xFFU) {
        return utf16ToUtf8(bytes, false, 2);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFFU &&
        static_cast<unsigned char>(bytes[1]) == 0xFEU) {
        return utf16ToUtf8(bytes, true, 2);
    }
    if (isValidUtf8(bytes)) {
        return bytes;
    }
    return cp1251ToUtf8(bytes);
}

std::string parsePdfLiteral(const std::string& data, std::size_t& i) {
    std::string bytes;
    int depth = 0;
    if (i >= data.size() || data[i] != '(') {
        return bytes;
    }
    ++i;
    ++depth;
    while (i < data.size() && depth > 0) {
        char ch = data[i++];
        if (ch == '\\' && i < data.size()) {
            char esc = data[i++];
            switch (esc) {
                case 'n': bytes.push_back('\n'); break;
                case 'r': bytes.push_back('\r'); break;
                case 't': bytes.push_back('\t'); break;
                case 'b': bytes.push_back('\b'); break;
                case 'f': bytes.push_back('\f'); break;
                case '\\': bytes.push_back('\\'); break;
                case '(' : bytes.push_back('('); break;
                case ')' : bytes.push_back(')'); break;
                case '\r':
                    if (i < data.size() && data[i] == '\n') {
                        ++i;
                    }
                    break;
                case '\n':
                    break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        int value = esc - '0';
                        int count = 1;
                        while (count < 3 && i < data.size() && data[i] >= '0' && data[i] <= '7') {
                            value = value * 8 + (data[i++] - '0');
                            ++count;
                        }
                        bytes.push_back(static_cast<char>(value));
                    } else {
                        bytes.push_back(esc);
                    }
                    break;
            }
        } else if (ch == '(') {
            ++depth;
            bytes.push_back(ch);
        } else if (ch == ')') {
            --depth;
            if (depth > 0) {
                bytes.push_back(ch);
            }
        } else {
            bytes.push_back(ch);
        }
    }
    return pdfBytesToText(bytes);
}

std::string parsePdfHexString(const std::string& data, std::size_t& i) {
    std::string bytes;
    if (i >= data.size() || data[i] != '<' || (i + 1 < data.size() && data[i + 1] == '<')) {
        return bytes;
    }
    ++i;
    int high = -1;
    while (i < data.size() && data[i] != '>') {
        const int value = hexValue(data[i++]);
        if (value < 0) {
            continue;
        }
        if (high < 0) {
            high = value;
        } else {
            bytes.push_back(static_cast<char>((high << 4) | value));
            high = -1;
        }
    }
    if (i < data.size() && data[i] == '>') {
        ++i;
    }
    if (high >= 0) {
        bytes.push_back(static_cast<char>(high << 4));
    }
    return pdfBytesToText(bytes);
}

std::string extractPdfFallback(const fs::path& path) {
    const auto maybe_data = readBinaryFile(path);
    if (!maybe_data) {
        return {};
    }
    const std::string& data = *maybe_data;
    std::string out;

    std::size_t pos = 0;
    while (true) {
        const std::size_t bt = data.find("BT", pos);
        if (bt == std::string::npos) {
            break;
        }
        const std::size_t et = data.find("ET", bt + 2);
        if (et == std::string::npos) {
            break;
        }
        std::size_t i = bt + 2;
        while (i < et) {
            if (data[i] == '(') {
                std::string text = parsePdfLiteral(data, i);
                if (!text.empty()) {
                    out += text;
                    out.push_back(' ');
                }
            } else if (data[i] == '<' && i + 1 < et && data[i + 1] != '<') {
                std::string text = parsePdfHexString(data, i);
                if (!text.empty()) {
                    out += text;
                    out.push_back(' ');
                }
            } else {
                if (data[i] == '*' || data[i] == '\'' || data[i] == '"') {
                    out.push_back('\n');
                }
                ++i;
            }
        }
        out.push_back('\n');
        pos = et + 2;
    }

    if (out.size() < 40) {
        std::size_t i = 0;
        while (i < data.size()) {
            if (data[i] == '(') {
                const std::string text = parsePdfLiteral(data, i);
                if (text.size() >= 3) {
                    out += text;
                    out.push_back('\n');
                }
            } else {
                ++i;
            }
        }
    }

    return normalizeNewlines(out);
}

std::string extractPdfText(const fs::path& path) {
    const PdfProbe probe = inspectPdf(path);
#ifdef _WIN32
    std::string text = runProcessCaptureW({
        L"pdftotext.exe",
        L"-layout",
        L"-enc",
        L"UTF-8",
        path.wstring(),
        L"-"
    }, probe.pdftotext_timeout_ms, probe.max_output_bytes);
    text = cleanPdfExtractedText(std::move(text));
    if (trim(text).size() >= 20) {
        return text;
    }
    if (!probe.try_mutool) {
        return {};
    }
    text = runProcessCaptureW({
        L"mutool.exe",
        L"draw",
        L"-F",
        L"txt",
        L"-o",
        L"-",
        path.wstring()
    }, probe.mutool_timeout_ms, probe.max_output_bytes);
    text = cleanPdfExtractedText(std::move(text));
    if (trim(text).size() >= 20) {
        return text;
    }
    if (!probe.try_fallback) {
        return {};
    }
    return cleanPdfExtractedText(extractPdfFallback(path));
#else
    const std::string p = pathToUtf8(path);
    std::string text = runCommandCapture("pdftotext -layout -enc UTF-8 " + shellDoubleQuote(p) + " -");
    text = cleanPdfExtractedText(std::move(text));
    if (trim(text).size() >= 20) {
        return text;
    }
    if (!probe.try_mutool) {
        return {};
    }
    text = runCommandCapture("mutool draw -F txt -o - " + shellDoubleQuote(p));
    text = cleanPdfExtractedText(std::move(text));
    if (trim(text).size() >= 20) {
        return text;
    }
    if (!probe.try_fallback) {
        return {};
    }
    return cleanPdfExtractedText(extractPdfFallback(path));
#endif
}

bool isSupportedExtension(const std::string& ext) {
    static const std::unordered_set<std::string> supported = {
        ".txt", ".text", ".md", ".markdown", ".docx", ".pdf"
    };
    return supported.count(lowerAscii(ext)) != 0;
}

std::string extractText(const fs::path& path) {
    const std::string ext = lowerAscii(path.extension().string());
    if (ext == ".txt" || ext == ".text") {
        const auto bytes = readBinaryFile(path);
        return bytes ? normalizeNewlines(bytesToUtf8Text(*bytes)) : std::string{};
    }
    if (ext == ".md" || ext == ".markdown") {
        const auto bytes = readBinaryFile(path);
        return bytes ? stripMarkdown(bytesToUtf8Text(*bytes)) : std::string{};
    }
    if (ext == ".docx") {
        return extractDocxText(path);
    }
    if (ext == ".pdf") {
        return extractPdfText(path);
    }
    return {};
}

std::vector<std::string> splitSentences(const std::string& text) {
    std::vector<std::string> sentences;
    std::size_t start = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        const bool boundary = ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == '\n';
        if (boundary && i + 1 - start >= 80) {
            std::string part = trim(text.substr(start, i + 1 - start));
            if (!part.empty()) {
                sentences.push_back(part);
            }
            start = i + 1;
        }
        ++i;
    }
    std::string tail = trim(text.substr(start));
    if (!tail.empty()) {
        sentences.push_back(tail);
    }
    return sentences;
}

std::vector<std::string> splitFragments(const std::string& text, std::size_t max_chars = 1400) {
    const std::string normalized = normalizeNewlines(text);
    std::vector<std::string> units;
    std::istringstream in(normalized);
    std::string line;
    std::string paragraph;

    auto flush_paragraph = [&]() {
        std::string p = trim(paragraph);
        if (!p.empty()) {
            if (p.size() > max_chars) {
                auto sentences = splitSentences(p);
                units.insert(units.end(), sentences.begin(), sentences.end());
            } else {
                units.push_back(p);
            }
        }
        paragraph.clear();
    };

    while (std::getline(in, line)) {
        if (trim(line).empty()) {
            flush_paragraph();
        } else {
            if (!paragraph.empty()) {
                paragraph.push_back('\n');
            }
            paragraph += line;
        }
    }
    flush_paragraph();

    std::vector<std::string> fragments;
    std::string current;
    for (const std::string& unit : units) {
        if (unit.size() > max_chars) {
            if (!current.empty()) {
                fragments.push_back(trim(current));
                current.clear();
            }
            for (std::size_t pos = 0; pos < unit.size(); pos += max_chars) {
                fragments.push_back(trim(unit.substr(pos, max_chars)));
            }
            continue;
        }
        if (!current.empty() && current.size() + unit.size() + 2 > max_chars) {
            fragments.push_back(trim(current));
            current.clear();
        }
        if (!current.empty()) {
            current += "\n\n";
        }
        current += unit;
    }
    if (!current.empty()) {
        fragments.push_back(trim(current));
    }
    return fragments;
}

std::unordered_map<std::string, std::vector<std::string>> makeAliasMap() {
    std::unordered_map<std::string, std::vector<std::string>> aliases;
    auto add_group = [&](std::initializer_list<std::string> words) {
        std::vector<std::string> normalized;
        for (const std::string& word : words) {
            const std::string norm = normalizeTerm(word);
            if (!norm.empty()) {
                normalized.push_back(norm);
            }
        }
        for (const std::string& word : normalized) {
            auto& list = aliases[word];
            for (const std::string& other : normalized) {
                if (other != word && std::find(list.begin(), list.end(), other) == list.end()) {
                    list.push_back(other);
                }
            }
        }
    };

    add_group({"поиск", "искать", "найти", "нахождение", "search", "find", "retrieve"});
    add_group({"смысл", "семантика", "значение", "контекст", "meaning", "semantic", "context"});
    add_group({"документ", "файл", "материал", "источник", "document", "file", "source"});
    add_group({"фрагмент", "абзац", "цитата", "отрывок", "paragraph", "snippet", "quote"});
    add_group({"договор", "контракт", "соглашение", "contract", "agreement"});
    add_group({"стоимость", "цена", "тариф", "сумма", "оплата", "payment", "price", "cost"});
    add_group({"срок", "дата", "дедлайн", "период", "time", "date", "deadline"});
    add_group({"ответственность", "штраф", "санкция", "penalty", "liability"});
    add_group({"клиент", "заказчик", "покупатель", "customer", "client"});
    add_group({"исполнитель", "поставщик", "подрядчик", "vendor", "supplier"});
    add_group({"модель", "нейросеть", "ml", "ai", "embedding", "эмбеддинг"});
    add_group({"pdf", "docx", "txt", "markdown", "md", "конспект"});
    return aliases;
}

int boundedLevenshtein(const std::vector<std::uint32_t>& a,
                       const std::vector<std::uint32_t>& b,
                       int limit) {
    const int m = static_cast<int>(a.size());
    const int n = static_cast<int>(b.size());
    if (std::abs(m - n) > limit) {
        return limit + 1;
    }

    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);
    for (int j = 0; j <= n; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        int row_min = curr[0];
        const int from = std::max(1, i - limit);
        const int to = std::min(n, i + limit);
        for (int j = 1; j < from; ++j) {
            curr[j] = limit + 1;
        }
        for (int j = from; j <= to; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
            row_min = std::min(row_min, curr[j]);
        }
        for (int j = to + 1; j <= n; ++j) {
            curr[j] = limit + 1;
        }
        if (row_min > limit) {
            return limit + 1;
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

std::string fileMetadataText(const fs::path& path, const std::string& type, bool content_indexed) {
    std::ostringstream out;
    const std::string filename = path.filename().empty() ? pathToUtf8(path) : pathToUtf8(path.filename());
    const std::string stem = path.stem().empty() ? filename : pathToUtf8(path.stem());
    out << "Имя файла: " << filename << "\n";
    out << "Название: " << stem << "\n";
    out << "Путь: " << pathToUtf8(path) << "\n";
    out << "Тип файла: " << type << "\n";
    if (!content_indexed) {
        out << "Текст документа не извлечен. Возможно, PDF состоит из сканов или фотографий без OCR. "
            << "Файл добавлен в индекс только по названию и пути.\n";
    }
    return out.str();
}

const std::unordered_map<std::string, std::vector<std::string>>& aliasMap() {
    static const std::unordered_map<std::string, std::vector<std::string>> aliases = makeAliasMap();
    return aliases;
}

struct PreparedFragmentData {
    std::uint32_t ordinal{};
    std::string text;
    std::uint32_t length{};
    bool metadata_only{};
    std::vector<std::pair<std::string, std::uint32_t>> term_counts;
    std::vector<std::string> unique_terms;
};

struct PreparedDocumentData {
    fs::path path;
    std::string type;
    bool content_indexed{};
    bool extraction_failed{};
    bool loaded_from_cache{};
    std::string extraction_error;
    std::vector<PreparedFragmentData> fragments;
};

PreparedFragmentData prepareFragmentData(std::string text, std::uint32_t ordinal, bool metadata_only) {
    PreparedFragmentData prepared;
    prepared.ordinal = ordinal;
    prepared.metadata_only = metadata_only;
    prepared.text = std::move(text);

    const auto tokens = tokenizeWithSpans(prepared.text);
    prepared.length = static_cast<std::uint32_t>(tokens.size());
    if (prepared.length == 0) {
        return prepared;
    }

    std::unordered_map<std::string, std::uint32_t> counts;
    counts.reserve(tokens.size());
    for (const TokenSpan& token : tokens) {
        ++counts[token.norm];
    }

    prepared.term_counts.reserve(counts.size());
    prepared.unique_terms.reserve(counts.size());
    for (auto& [term, tf] : counts) {
        prepared.unique_terms.push_back(term);
        prepared.term_counts.emplace_back(std::move(term), tf);
    }
    std::sort(prepared.unique_terms.begin(), prepared.unique_terms.end());
    return prepared;
}

PreparedDocumentData prepareDocumentData(const fs::path& path) {
    PreparedDocumentData prepared;
    prepared.path = path;
    prepared.type = lowerAscii(path.extension().string());

    std::string text;
    try {
        text = extractText(path);
    } catch (const std::exception& ex) {
        prepared.extraction_failed = true;
        prepared.extraction_error = ex.what();
    }

    prepared.content_indexed = !trim(text).empty();

    PreparedFragmentData metadata = prepareFragmentData(
        fileMetadataText(path, prepared.type, prepared.content_indexed),
        0,
        true
    );
    if (metadata.length > 0) {
        prepared.fragments.push_back(std::move(metadata));
    }

    std::uint32_t ordinal = 1;
    for (const std::string& part : splitFragments(text)) {
        PreparedFragmentData fragment = prepareFragmentData(part, ordinal, false);
        if (fragment.length == 0) {
            continue;
        }
        prepared.fragments.push_back(std::move(fragment));
        ++ordinal;
    }

    return prepared;
}

std::size_t defaultBuildThreadCount(std::size_t requested = 0) {
    if (requested != 0) {
        return std::max<std::size_t>(1, requested);
    }
    const unsigned int hc = std::thread::hardware_concurrency();
    const std::size_t derived = hc == 0 ? 4U : static_cast<std::size_t>(hc);
    return std::clamp<std::size_t>(derived, 1, 8);
}

struct CacheManifestEntry {
    std::uint64_t size{};
    std::int64_t mtime_ticks{};
    std::string cache_name;
};

struct BuildCache {
    std::unordered_map<std::string, CacheManifestEntry> entries;
};

struct FileJob {
    fs::path path;
    std::string relative_key;
    std::uint64_t size{};
    std::int64_t mtime_ticks{};
    std::string cache_name;
};

std::int64_t fileMtimeTicks(const fs::path& path) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(mtime.time_since_epoch().count());
}

std::uint64_t fileSizeOrZero(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0ULL : static_cast<std::uint64_t>(size);
}

std::string relativePathKey(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, root, ec);
    return pathToUtf8(ec ? path : relative);
}

std::string hashedCacheFileName(const std::string& key) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << stableHash64(key) << ".bin";
    return out.str();
}

fs::path buildCacheDirForRoot(const fs::path& root) {
    std::error_code ec;
    fs::path base = fs::current_path(ec);
    if (ec || base.empty()) {
        base = ".";
    }
    fs::path normalized = fs::weakly_canonical(root, ec);
    if (ec) {
        normalized = root.lexically_normal();
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << stableHash64(pathToUtf8(normalized));
    return base / ".docsearch_cache" / out.str();
}

bool shouldSkipDirectoryName(const fs::path& path) {
    const std::string name = lowerAscii(path.filename().string());
    static const std::unordered_set<std::string> skipped = {
        ".docsearch_cache",
        ".git",
        ".hg",
        ".svn",
        ".idea",
        ".vs",
        "build",
        "cmake-build-debug",
        "cmake-build-release",
        "node_modules",
        ".venv",
        "venv",
        "__pycache__"
    };
    return skipped.count(name) != 0;
}

BuildCache loadBuildCache(const fs::path& cache_dir) {
    BuildCache cache;
    std::ifstream in(cache_dir / "manifest.bin", std::ios::binary);
    if (!in) {
        return cache;
    }

    std::string magic;
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    if (!readSizedString(in, magic) || magic != "DOCSEARCH_CACHE_V1" ||
        !readPod<std::uint32_t>(in, version) || version != 1 ||
        !readPod<std::uint64_t>(in, count)) {
        return {};
    }

    cache.entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string key;
        CacheManifestEntry entry;
        if (!readSizedString(in, key) ||
            !readPod<std::uint64_t>(in, entry.size) ||
            !readPod<std::int64_t>(in, entry.mtime_ticks) ||
            !readSizedString(in, entry.cache_name)) {
            return {};
        }
        cache.entries.emplace(std::move(key), std::move(entry));
    }
    return cache;
}

bool saveBuildCache(const fs::path& cache_dir, const BuildCache& cache) {
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    std::ofstream out(cache_dir / "manifest.bin", std::ios::binary);
    if (!out) {
        return false;
    }
    writeSizedString(out, "DOCSEARCH_CACHE_V1");
    writePod<std::uint32_t>(out, 1);
    writePod<std::uint64_t>(out, static_cast<std::uint64_t>(cache.entries.size()));
    for (const auto& [key, entry] : cache.entries) {
        writeSizedString(out, key);
        writePod<std::uint64_t>(out, entry.size);
        writePod<std::int64_t>(out, entry.mtime_ticks);
        writeSizedString(out, entry.cache_name);
    }
    return static_cast<bool>(out);
}

bool savePreparedDocumentCache(const fs::path& cache_file, const PreparedDocumentData& prepared) {
    std::error_code ec;
    fs::create_directories(cache_file.parent_path(), ec);
    std::ofstream out(cache_file, std::ios::binary);
    if (!out) {
        return false;
    }

    writeSizedString(out, "DOCSEARCH_PREPARED_V1");
    writePod<std::uint32_t>(out, 1);
    writeSizedString(out, pathToUtf8(prepared.path));
    writeSizedString(out, prepared.type);
    writePod<std::uint8_t>(out, prepared.content_indexed ? 1 : 0);
    writePod<std::uint8_t>(out, prepared.extraction_failed ? 1 : 0);
    writeSizedString(out, prepared.extraction_error);
    writePod<std::uint64_t>(out, static_cast<std::uint64_t>(prepared.fragments.size()));
    for (const PreparedFragmentData& fragment : prepared.fragments) {
        writePod<std::uint32_t>(out, fragment.ordinal);
        writePod<std::uint32_t>(out, fragment.length);
        writePod<std::uint8_t>(out, fragment.metadata_only ? 1 : 0);
        writeSizedString(out, fragment.text);
        writePod<std::uint64_t>(out, static_cast<std::uint64_t>(fragment.term_counts.size()));
        for (const auto& [term, tf] : fragment.term_counts) {
            writeSizedString(out, term);
            writePod<std::uint32_t>(out, tf);
        }
        writePod<std::uint64_t>(out, static_cast<std::uint64_t>(fragment.unique_terms.size()));
        for (const std::string& term : fragment.unique_terms) {
            writeSizedString(out, term);
        }
    }
    return static_cast<bool>(out);
}

bool loadPreparedDocumentCache(const fs::path& cache_file, PreparedDocumentData& prepared) {
    std::ifstream in(cache_file, std::ios::binary);
    if (!in) {
        return false;
    }

    std::string magic;
    std::uint32_t version = 0;
    if (!readSizedString(in, magic) || magic != "DOCSEARCH_PREPARED_V1" ||
        !readPod<std::uint32_t>(in, version) || version != 1) {
        return false;
    }

    std::string path_utf8;
    std::uint8_t content_indexed = 0;
    std::uint8_t extraction_failed = 0;
    if (!readSizedString(in, path_utf8) ||
        !readSizedString(in, prepared.type) ||
        !readPod<std::uint8_t>(in, content_indexed) ||
        !readPod<std::uint8_t>(in, extraction_failed) ||
        !readSizedString(in, prepared.extraction_error)) {
        return false;
    }

    prepared.path = pathFromUtf8(path_utf8);
    prepared.content_indexed = content_indexed != 0;
    prepared.extraction_failed = extraction_failed != 0;
    prepared.loaded_from_cache = true;

    std::uint64_t fragment_count = 0;
    if (!readPod<std::uint64_t>(in, fragment_count)) {
        return false;
    }
    prepared.fragments.clear();
    prepared.fragments.reserve(static_cast<std::size_t>(fragment_count));
    for (std::uint64_t i = 0; i < fragment_count; ++i) {
        PreparedFragmentData fragment;
        std::uint8_t metadata_only = 0;
        std::uint64_t term_count = 0;
        std::uint64_t unique_term_count = 0;
        if (!readPod<std::uint32_t>(in, fragment.ordinal) ||
            !readPod<std::uint32_t>(in, fragment.length) ||
            !readPod<std::uint8_t>(in, metadata_only) ||
            !readSizedString(in, fragment.text) ||
            !readPod<std::uint64_t>(in, term_count)) {
            return false;
        }
        fragment.metadata_only = metadata_only != 0;
        fragment.term_counts.reserve(static_cast<std::size_t>(term_count));
        for (std::uint64_t j = 0; j < term_count; ++j) {
            std::string term;
            std::uint32_t tf = 0;
            if (!readSizedString(in, term) || !readPod<std::uint32_t>(in, tf)) {
                return false;
            }
            fragment.term_counts.emplace_back(std::move(term), tf);
        }
        if (!readPod<std::uint64_t>(in, unique_term_count)) {
            return false;
        }
        fragment.unique_terms.reserve(static_cast<std::size_t>(unique_term_count));
        for (std::uint64_t j = 0; j < unique_term_count; ++j) {
            std::string term;
            if (!readSizedString(in, term)) {
                return false;
            }
            fragment.unique_terms.push_back(std::move(term));
        }
        prepared.fragments.push_back(std::move(fragment));
    }
    return true;
}

std::vector<std::string> utf8Chars(const std::string& text) {
    std::vector<std::string> chars;
    chars.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        std::uint32_t cp = 0;
        if (!decodeNextUtf8(text, i, cp)) {
            break;
        }
        chars.push_back(text.substr(start, i - start));
    }
    return chars;
}

std::vector<std::string> ngramsForTerm(const std::string& term) {
    const auto chars = utf8Chars(term);
    if (chars.empty()) {
        return {};
    }

    const std::size_t gram_size = chars.size() < 3 ? chars.size() : 3;
    std::vector<std::string> grams;
    grams.reserve(chars.size());
    for (std::size_t i = 0; i + gram_size <= chars.size(); ++i) {
        std::string gram;
        for (std::size_t j = 0; j < gram_size; ++j) {
            if (j != 0) {
                gram.push_back('\x1F');
            }
            gram += chars[i + j];
        }
        grams.push_back(std::move(gram));
    }
    if (grams.empty()) {
        grams.push_back(term);
    }
    std::sort(grams.begin(), grams.end());
    grams.erase(std::unique(grams.begin(), grams.end()), grams.end());
    return grams;
}

class SearchIndex {
public:
    void addDocument(const fs::path& path, const std::string& type, const std::string& text) {
        PreparedDocumentData prepared;
        prepared.path = path;
        prepared.type = type;
        prepared.content_indexed = !trim(text).empty();

        PreparedFragmentData metadata = prepareFragmentData(
            fileMetadataText(path, type, prepared.content_indexed),
            0,
            true
        );
        if (metadata.length > 0) {
            prepared.fragments.push_back(std::move(metadata));
        }

        std::uint32_t ordinal = 1;
        for (const std::string& part : splitFragments(text)) {
            PreparedFragmentData fragment = prepareFragmentData(part, ordinal, false);
            if (fragment.length == 0) {
                continue;
            }
            prepared.fragments.push_back(std::move(fragment));
            ++ordinal;
        }

        addPreparedDocument(std::move(prepared));
        rebuild();
    }

    void rebuild() {
        postings_.clear();
        doc_postings_.clear();
        segments_.clear();
        vocabulary_.clear();
        fragment_terms_.clear();
        document_terms_.clear();
        fragment_terms_.reserve(fragments_.size());
        std::uint64_t total_len = 0;

        for (Fragment& fragment : fragments_) {
            auto tokens = tokenizeWithSpans(fragment.text);
            fragment.length = static_cast<std::uint32_t>(tokens.size());
            total_len += fragment.length;

            std::unordered_map<std::string, std::uint32_t> counts;
            counts.reserve(tokens.size());
            for (const TokenSpan& token : tokens) {
                ++counts[token.norm];
            }
            std::vector<std::string> unique_terms;
            unique_terms.reserve(counts.size());
            for (const auto& [term, tf] : counts) {
                postings_[term].push_back(Posting{fragment.id, tf});
                vocabulary_.insert(term);
                unique_terms.push_back(term);
            }
            std::sort(unique_terms.begin(), unique_terms.end());
            fragment_terms_.push_back(std::move(unique_terms));
        }

        avg_fragment_len_ = fragments_.empty() ? 1.0 : static_cast<double>(total_len) / fragments_.size();
        if (avg_fragment_len_ <= 0.0) {
            avg_fragment_len_ = 1.0;
        }
        rebuildDocumentAndSegmentIndices();
        rebuildAuxiliaryIndices();
    }

    bool buildFromFolder(const fs::path& root, std::size_t requested_threads = 0) {
        documents_.clear();
        fragments_.clear();
        postings_.clear();
        doc_postings_.clear();
        segments_.clear();
        vocabulary_.clear();
        fragment_terms_.clear();
        document_terms_.clear();
        vocabulary_terms_.clear();
        ngram_index_.clear();
        avg_fragment_len_ = 1.0;
        avg_doc_len_ = 1.0;

        if (!fs::exists(root)) {
            std::cerr << "Folder does not exist: " << pathToUtf8(root) << "\n";
            return false;
        }

        const fs::path cache_dir = buildCacheDirForRoot(root);
        const BuildCache existing_cache = loadBuildCache(cache_dir);
        std::vector<FileJob> files;
        const auto options = fs::directory_options::skip_permission_denied;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, options, ec), end; it != end; ) {
            const fs::directory_entry entry = *it;
            ec.clear();
            if (entry.is_directory(ec)) {
                if (!ec && shouldSkipDirectoryName(entry.path())) {
                    it.disable_recursion_pending();
                }
                ec.clear();
                it.increment(ec);
                if (ec) {
                    ec.clear();
                }
                continue;
            }

            if (!entry.is_regular_file(ec)) {
                ec.clear();
                it.increment(ec);
                if (ec) {
                    ec.clear();
                }
                continue;
            }

            const std::string ext = entry.path().extension().string();
            if (isSupportedExtension(ext)) {
                FileJob job;
                job.path = entry.path();
                job.relative_key = relativePathKey(root, job.path);
                job.size = fileSizeOrZero(job.path);
                job.mtime_ticks = fileMtimeTicks(job.path);
                job.cache_name = hashedCacheFileName(job.relative_key);
                files.push_back(std::move(job));
            }

            ec.clear();
            it.increment(ec);
            if (ec) {
                ec.clear();
            }
        }
        std::sort(files.begin(), files.end(), [](const FileJob& a, const FileJob& b) {
            return a.path < b.path;
        });

        std::cerr << "Found supported files: " << files.size() << "\n";
        if (files.empty()) {
            return false;
        }

        std::vector<PreparedDocumentData> prepared(files.size());
        std::vector<std::optional<CacheManifestEntry>> manifest_updates(files.size());
        const std::size_t worker_count = std::min(files.size(), defaultBuildThreadCount(requested_threads));
        std::atomic<std::size_t> next_index{0};
        std::atomic<std::size_t> completed{0};
        std::atomic<std::size_t> cache_hits{0};
        std::mutex log_mutex;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&, total = files.size()]() {
                while (true) {
                    const std::size_t idx = next_index.fetch_add(1);
                    if (idx >= total) {
                        break;
                    }
                    const FileJob& job = files[idx];
                    PreparedDocumentData current;

                    try {
                        bool cache_hit = false;
                        const auto cache_it = existing_cache.entries.find(job.relative_key);
                        if (cache_it != existing_cache.entries.end() &&
                            cache_it->second.size == job.size &&
                            cache_it->second.mtime_ticks == job.mtime_ticks) {
                            cache_hit = loadPreparedDocumentCache(cache_dir / cache_it->second.cache_name, current);
                        }

                        if (!cache_hit) {
                            current = prepareDocumentData(job.path);
                            if (savePreparedDocumentCache(cache_dir / job.cache_name, current)) {
                                manifest_updates[idx] = CacheManifestEntry{job.size, job.mtime_ticks, job.cache_name};
                            }
                        } else {
                            manifest_updates[idx] = CacheManifestEntry{job.size, job.mtime_ticks, job.cache_name};
                            cache_hits.fetch_add(1);
                        }
                    } catch (const std::exception& ex) {
                        current.path = job.path;
                        current.type = lowerAscii(job.path.extension().string());
                        current.content_indexed = false;
                        current.extraction_failed = true;
                        current.extraction_error = ex.what();
                        PreparedFragmentData metadata = prepareFragmentData(
                            fileMetadataText(job.path, current.type, false),
                            0,
                            true
                        );
                        if (metadata.length > 0) {
                            current.fragments.push_back(std::move(metadata));
                        }
                    }

                    prepared[idx] = std::move(current);
                    const std::size_t done = completed.fetch_add(1) + 1;

                    std::lock_guard<std::mutex> guard(log_mutex);
                    std::cerr << "[" << done << "/" << total << "] " << pathToUtf8(job.path);
                    if (prepared[idx].loaded_from_cache) {
                        std::cerr << " [cache]";
                    }
                    std::cerr << "\n";
                    if (prepared[idx].extraction_failed) {
                        std::cerr << "  extraction error: " << prepared[idx].extraction_error << "\n";
                    }
                    if (!prepared[idx].content_indexed) {
                        if (prepared[idx].type == ".pdf") {
                            std::cerr << "  no extractable text; possibly scanned/image-only PDF, indexed filename only\n";
                        } else {
                            std::cerr << "  no extractable text; indexed filename only\n";
                        }
                    }
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }

        BuildCache updated_cache;
        updated_cache.entries.reserve(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (manifest_updates[i].has_value()) {
                updated_cache.entries.emplace(files[i].relative_key, *manifest_updates[i]);
            }
        }
        if (!updated_cache.entries.empty() && !saveBuildCache(cache_dir, updated_cache)) {
            std::cerr << "Warning: cannot save build cache manifest: " << pathToUtf8(cache_dir / "manifest.bin") << "\n";
        }

        documents_.reserve(prepared.size());
        std::size_t total_fragments = 0;
        for (const PreparedDocumentData& doc : prepared) {
            total_fragments += doc.fragments.size();
        }
        fragments_.reserve(total_fragments);
        fragment_terms_.reserve(total_fragments);

        std::uint64_t total_len = 0;
        for (PreparedDocumentData& doc : prepared) {
            total_len += addPreparedDocument(std::move(doc));
        }

        avg_fragment_len_ = fragments_.empty() ? 1.0 : static_cast<double>(total_len) / fragments_.size();
        if (avg_fragment_len_ <= 0.0) {
            avg_fragment_len_ = 1.0;
        }
        rebuildDocumentAndSegmentIndices();
        rebuildAuxiliaryIndices();
        std::cerr << "Cache hits: " << cache_hits.load() << "/" << files.size() << "\n";
        return !fragments_.empty();
    }

    std::vector<SearchResult> search(const std::string& query, std::size_t limit) const {
        const std::vector<std::string> base_terms = queryBaseTerms(query);
        if (base_terms.empty() || fragments_.empty() || documents_.empty()) {
            return {};
        }

        const auto expanded = expandQuery(base_terms);
        std::unordered_map<std::uint32_t, double> doc_scores;
        const double n_docs = static_cast<double>(documents_.size());
        constexpr double k1 = 1.45;
        constexpr double b = 0.72;

        for (const auto& [term, q_weight] : expanded) {
            const auto it = doc_postings_.find(term);
            if (it == doc_postings_.end() || it->second.empty()) {
                continue;
            }
            const double df = static_cast<double>(it->second.size());
            const double idf = std::log(1.0 + (n_docs - df + 0.5) / (df + 0.5));
            for (const DocPosting& posting : it->second) {
                const Document& doc = documents_.at(posting.doc_id);
                const double tf = static_cast<double>(posting.tf);
                const double denom = tf + k1 * (1.0 - b + b * doc.length / avg_doc_len_);
                doc_scores[posting.doc_id] += q_weight * idf * (tf * (k1 + 1.0)) / denom;
            }
        }

        const std::string lowered_query = trim(lowerUtf8(query));
        for (auto& [doc_id, score] : doc_scores) {
            const Document& doc = documents_.at(doc_id);
            const auto& doc_terms = document_terms_.at(doc_id);
            int covered = 0;
            for (const std::string& term : base_terms) {
                if (std::binary_search(doc_terms.begin(), doc_terms.end(), term)) {
                    ++covered;
                }
            }
            const double coverage = static_cast<double>(covered) / static_cast<double>(base_terms.size());
            score *= 0.88 + 0.34 * coverage;

            const std::string title = lowerUtf8(doc.title);
            const std::string path = lowerUtf8(doc.path);
            if (lowered_query.size() >= 4 && title.find(lowered_query) != std::string::npos) {
                score += 2.25;
            }
            if (lowered_query.size() >= 4 && path.find(lowered_query) != std::string::npos) {
                score += 1.0;
            }
        }

        if (doc_scores.empty()) {
            return {};
        }

        std::vector<std::pair<std::uint32_t, double>> ranked_docs;
        ranked_docs.reserve(doc_scores.size());
        for (const auto& [doc_id, score] : doc_scores) {
            if (score > 0.0) {
                ranked_docs.emplace_back(doc_id, score);
            }
        }
        std::sort(ranked_docs.begin(), ranked_docs.end(), [](const auto& a, const auto& b) {
            if (std::fabs(a.second - b.second) > 1e-9) {
                return a.second > b.second;
            }
            return a.first < b.first;
        });
        if (ranked_docs.empty()) {
            return {};
        }

        const std::size_t doc_limit = std::min<std::size_t>(
            ranked_docs.size(),
            std::max<std::size_t>(limit * 8, 24)
        );

        std::unordered_map<std::uint32_t, double> segment_scores;
        for (std::size_t i = 0; i < doc_limit; ++i) {
            const Document& doc = documents_.at(ranked_docs[i].first);
            segment_scores[doc.segment_id] += ranked_docs[i].second;
        }

        std::vector<std::pair<std::uint32_t, double>> ranked_segments;
        ranked_segments.reserve(segment_scores.size());
        for (const auto& [segment_id, score] : segment_scores) {
            ranked_segments.emplace_back(segment_id, score);
        }
        std::sort(ranked_segments.begin(), ranked_segments.end(), [](const auto& a, const auto& b) {
            if (std::fabs(a.second - b.second) > 1e-9) {
                return a.second > b.second;
            }
            return a.first < b.first;
        });

        const std::size_t segment_limit = std::min<std::size_t>(
            ranked_segments.size(),
            std::max<std::size_t>(limit / 2 + 1, 3)
        );
        std::unordered_set<std::uint32_t> allowed_segments;
        allowed_segments.reserve(segment_limit);
        for (std::size_t i = 0; i < segment_limit; ++i) {
            allowed_segments.insert(ranked_segments[i].first);
        }

        std::unordered_map<std::uint32_t, double> candidate_doc_scores;
        candidate_doc_scores.reserve(doc_limit);
        for (const auto& [doc_id, score] : ranked_docs) {
            const Document& doc = documents_.at(doc_id);
            if (allowed_segments.count(doc.segment_id) == 0) {
                continue;
            }
            candidate_doc_scores.emplace(doc_id, score);
            if (candidate_doc_scores.size() >= doc_limit) {
                break;
            }
        }

        std::unordered_map<std::uint32_t, double> scores;
        const double n = static_cast<double>(fragments_.size());
        for (const auto& [term, q_weight] : expanded) {
            const auto it = postings_.find(term);
            if (it == postings_.end() || it->second.empty()) {
                continue;
            }
            const double df = static_cast<double>(it->second.size());
            const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
            for (const Posting& posting : it->second) {
                const Fragment& fragment = fragments_.at(posting.fragment_id);
                const auto doc_score_it = candidate_doc_scores.find(fragment.doc_id);
                if (doc_score_it == candidate_doc_scores.end()) {
                    continue;
                }
                const double tf = static_cast<double>(posting.tf);
                const double denom = tf + k1 * (1.0 - b + b * fragment.length / avg_fragment_len_);
                scores[posting.fragment_id] += q_weight * idf * (tf * (k1 + 1.0)) / denom;
            }
        }

        for (auto& [fragment_id, score] : scores) {
            const Fragment& fragment = fragments_.at(fragment_id);
            const auto& fragment_terms = fragment_terms_.at(fragment_id);
            const auto doc_score_it = candidate_doc_scores.find(fragment.doc_id);
            if (doc_score_it == candidate_doc_scores.end()) {
                continue;
            }

            int covered = 0;
            for (const std::string& term : base_terms) {
                if (std::binary_search(fragment_terms.begin(), fragment_terms.end(), term)) {
                    ++covered;
                }
            }
            const double coverage = static_cast<double>(covered) / static_cast<double>(base_terms.size());
            score *= 0.85 + 0.30 * coverage;
            score += doc_score_it->second * (fragment.metadata_only ? 0.10 : 0.18);

            if (lowered_query.size() >= 4 && lowerUtf8(fragment.text).find(lowered_query) != std::string::npos) {
                score += 1.25;
            }
            if (fragment.metadata_only) {
                score *= 0.72;
                const Document& doc = documents_.at(fragment.doc_id);
                const std::string title = lowerUtf8(doc.title);
                if (lowered_query.size() >= 3 && title.find(lowered_query) != std::string::npos) {
                    score += 3.0;
                }
            }
        }

        std::vector<SearchResult> results;
        results.reserve(scores.size());
        for (const auto& [fragment_id, score] : scores) {
            if (score > 0.0) {
                results.push_back(SearchResult{fragment_id, score});
            }
        }
        std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
            if (std::fabs(a.score - b.score) > 1e-9) {
                return a.score > b.score;
            }
            return a.fragment_id < b.fragment_id;
        });
        if (results.size() > limit) {
            results.resize(limit);
        }
        return results;
    }

    std::unordered_set<std::string> highlightTerms(const std::string& query) const {
        std::unordered_set<std::string> terms;
        const auto base = queryBaseTerms(query);
        const auto expanded = expandQuery(base);
        for (const auto& [term, _] : expanded) {
            terms.insert(term);
        }
        return terms;
    }

    bool save(const fs::path& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        const std::string magic = "DOCSEARCH_INDEX_V1";
        writeString(out, magic);
        writeNumber<std::uint32_t>(out, 4);
        writeNumber<std::uint64_t>(out, documents_.size());
        for (const Document& doc : documents_) {
            writeString(out, doc.path);
            writeString(out, doc.title);
            writeString(out, doc.type);
            writeNumber<std::uint32_t>(out, doc.length);
            writeNumber<std::uint32_t>(out, doc.first_fragment_id);
            writeNumber<std::uint32_t>(out, doc.fragment_count);
            writeNumber<std::uint32_t>(out, doc.segment_id);
        }
        writeNumber<std::uint64_t>(out, fragments_.size());
        for (const Fragment& fragment : fragments_) {
            writeNumber<std::uint32_t>(out, fragment.doc_id);
            writeNumber<std::uint32_t>(out, fragment.ordinal);
            writeNumber<std::uint8_t>(out, fragment.metadata_only ? 1 : 0);
            writeNumber<std::uint32_t>(out, fragment.length);
            writeString(out, fragment.text);
        }
        writeNumber<double>(out, avg_fragment_len_);
        writeNumber<std::uint64_t>(out, fragment_terms_.size());
        for (const auto& terms : fragment_terms_) {
            writeNumber<std::uint64_t>(out, terms.size());
            for (const std::string& term : terms) {
                writeString(out, term);
            }
        }
        writeNumber<std::uint64_t>(out, postings_.size());
        for (const auto& [term, postings] : postings_) {
            writeString(out, term);
            writeNumber<std::uint64_t>(out, postings.size());
            for (const Posting& posting : postings) {
                writeNumber<std::uint32_t>(out, posting.fragment_id);
                writeNumber<std::uint32_t>(out, posting.tf);
            }
        }
        writeNumber<double>(out, avg_doc_len_);
        writeNumber<std::uint64_t>(out, document_terms_.size());
        for (const auto& terms : document_terms_) {
            writeNumber<std::uint64_t>(out, terms.size());
            for (const std::string& term : terms) {
                writeString(out, term);
            }
        }
        writeNumber<std::uint64_t>(out, doc_postings_.size());
        for (const auto& [term, postings] : doc_postings_) {
            writeString(out, term);
            writeNumber<std::uint64_t>(out, postings.size());
            for (const DocPosting& posting : postings) {
                writeNumber<std::uint32_t>(out, posting.doc_id);
                writeNumber<std::uint32_t>(out, posting.tf);
            }
        }
        writeNumber<std::uint64_t>(out, segments_.size());
        for (const Segment& segment : segments_) {
            writeNumber<std::uint32_t>(out, segment.id);
            writeNumber<std::uint32_t>(out, segment.first_doc_id);
            writeNumber<std::uint32_t>(out, segment.doc_count);
            writeNumber<std::uint32_t>(out, segment.first_fragment_id);
            writeNumber<std::uint32_t>(out, segment.fragment_count);
            writeNumber<std::uint32_t>(out, segment.length);
        }
        return static_cast<bool>(out);
    }

    bool load(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }

        std::string magic;
        if (!readString(in, magic) || magic != "DOCSEARCH_INDEX_V1") {
            return false;
        }
        std::uint32_t version = 0;
        if (!readNumber<std::uint32_t>(in, version) || (version < 1 || version > 4)) {
            return false;
        }

        documents_.clear();
        fragments_.clear();
        segments_.clear();
        document_terms_.clear();
        postings_.clear();
        doc_postings_.clear();
        vocabulary_.clear();
        fragment_terms_.clear();
        vocabulary_terms_.clear();
        ngram_index_.clear();
        avg_fragment_len_ = 1.0;
        avg_doc_len_ = 1.0;

        std::uint64_t doc_count = 0;
        if (!readNumber<std::uint64_t>(in, doc_count)) {
            return false;
        }
        documents_.reserve(static_cast<std::size_t>(doc_count));
        for (std::uint64_t i = 0; i < doc_count; ++i) {
            Document doc;
            doc.id = static_cast<std::uint32_t>(i);
            if (!readString(in, doc.path) || !readString(in, doc.title) || !readString(in, doc.type)) {
                return false;
            }
            if (version >= 4) {
                if (!readNumber<std::uint32_t>(in, doc.length) ||
                    !readNumber<std::uint32_t>(in, doc.first_fragment_id) ||
                    !readNumber<std::uint32_t>(in, doc.fragment_count) ||
                    !readNumber<std::uint32_t>(in, doc.segment_id)) {
                    return false;
                }
            }
            documents_.push_back(std::move(doc));
        }

        std::uint64_t fragment_count = 0;
        if (!readNumber<std::uint64_t>(in, fragment_count)) {
            return false;
        }
        fragments_.reserve(static_cast<std::size_t>(fragment_count));
        for (std::uint64_t i = 0; i < fragment_count; ++i) {
            Fragment fragment;
            fragment.id = static_cast<std::uint32_t>(i);
            if (!readNumber<std::uint32_t>(in, fragment.doc_id) ||
                !readNumber<std::uint32_t>(in, fragment.ordinal)) {
                return false;
            }
            if (version >= 2) {
                std::uint8_t metadata_only = 0;
                if (!readNumber<std::uint8_t>(in, metadata_only)) {
                    return false;
                }
                fragment.metadata_only = metadata_only != 0;
            }
            if (version >= 3) {
                if (!readNumber<std::uint32_t>(in, fragment.length)) {
                    return false;
                }
            }
            if (!readString(in, fragment.text)) {
                return false;
            }
            fragments_.push_back(std::move(fragment));
        }

        if (version >= 3) {
            if (!readNumber<double>(in, avg_fragment_len_)) {
                return false;
            }
            std::uint64_t fragment_terms_count = 0;
            if (!readNumber<std::uint64_t>(in, fragment_terms_count) ||
                fragment_terms_count != fragments_.size()) {
                return false;
            }
            fragment_terms_.reserve(static_cast<std::size_t>(fragment_terms_count));
            for (std::uint64_t i = 0; i < fragment_terms_count; ++i) {
                std::uint64_t term_count = 0;
                if (!readNumber<std::uint64_t>(in, term_count)) {
                    return false;
                }
                std::vector<std::string> terms;
                terms.reserve(static_cast<std::size_t>(term_count));
                for (std::uint64_t j = 0; j < term_count; ++j) {
                    std::string term;
                    if (!readString(in, term)) {
                        return false;
                    }
                    terms.push_back(std::move(term));
                }
                fragment_terms_.push_back(std::move(terms));
            }

            std::uint64_t posting_terms = 0;
            if (!readNumber<std::uint64_t>(in, posting_terms)) {
                return false;
            }
            for (std::uint64_t i = 0; i < posting_terms; ++i) {
                std::string term;
                if (!readString(in, term)) {
                    return false;
                }
                std::uint64_t posting_count = 0;
                if (!readNumber<std::uint64_t>(in, posting_count)) {
                    return false;
                }
                auto& list = postings_[term];
                list.reserve(static_cast<std::size_t>(posting_count));
                vocabulary_.insert(term);
                for (std::uint64_t j = 0; j < posting_count; ++j) {
                    Posting posting;
                    if (!readNumber<std::uint32_t>(in, posting.fragment_id) ||
                        !readNumber<std::uint32_t>(in, posting.tf)) {
                        return false;
                    }
                    list.push_back(posting);
                }
            }
            if (version >= 4) {
                if (!readNumber<double>(in, avg_doc_len_)) {
                    return false;
                }
                std::uint64_t document_terms_count = 0;
                if (!readNumber<std::uint64_t>(in, document_terms_count) ||
                    document_terms_count != documents_.size()) {
                    return false;
                }
                document_terms_.reserve(static_cast<std::size_t>(document_terms_count));
                for (std::uint64_t i = 0; i < document_terms_count; ++i) {
                    std::uint64_t term_count = 0;
                    if (!readNumber<std::uint64_t>(in, term_count)) {
                        return false;
                    }
                    std::vector<std::string> terms;
                    terms.reserve(static_cast<std::size_t>(term_count));
                    for (std::uint64_t j = 0; j < term_count; ++j) {
                        std::string term;
                        if (!readString(in, term)) {
                            return false;
                        }
                        terms.push_back(std::move(term));
                    }
                    document_terms_.push_back(std::move(terms));
                }

                std::uint64_t doc_posting_terms = 0;
                if (!readNumber<std::uint64_t>(in, doc_posting_terms)) {
                    return false;
                }
                for (std::uint64_t i = 0; i < doc_posting_terms; ++i) {
                    std::string term;
                    if (!readString(in, term)) {
                        return false;
                    }
                    std::uint64_t posting_count = 0;
                    if (!readNumber<std::uint64_t>(in, posting_count)) {
                        return false;
                    }
                    auto& list = doc_postings_[term];
                    list.reserve(static_cast<std::size_t>(posting_count));
                    for (std::uint64_t j = 0; j < posting_count; ++j) {
                        DocPosting posting;
                        if (!readNumber<std::uint32_t>(in, posting.doc_id) ||
                            !readNumber<std::uint32_t>(in, posting.tf)) {
                            return false;
                        }
                        list.push_back(posting);
                    }
                }

                std::uint64_t segment_count = 0;
                if (!readNumber<std::uint64_t>(in, segment_count)) {
                    return false;
                }
                segments_.reserve(static_cast<std::size_t>(segment_count));
                for (std::uint64_t i = 0; i < segment_count; ++i) {
                    Segment segment;
                    if (!readNumber<std::uint32_t>(in, segment.id) ||
                        !readNumber<std::uint32_t>(in, segment.first_doc_id) ||
                        !readNumber<std::uint32_t>(in, segment.doc_count) ||
                        !readNumber<std::uint32_t>(in, segment.first_fragment_id) ||
                        !readNumber<std::uint32_t>(in, segment.fragment_count) ||
                        !readNumber<std::uint32_t>(in, segment.length)) {
                        return false;
                    }
                    segments_.push_back(segment);
                }
            } else {
                rebuildDocumentAndSegmentIndices();
            }
        } else {
            rebuild();
        }

        rebuildAuxiliaryIndices();
        return true;
    }

    const Document& document(std::uint32_t id) const {
        return documents_.at(id);
    }

    const Fragment& fragment(std::uint32_t id) const {
        return fragments_.at(id);
    }

    std::size_t documentCount() const {
        return documents_.size();
    }

    std::size_t fragmentCount() const {
        return fragments_.size();
    }

    std::size_t segmentCount() const {
        return segments_.size();
    }

private:
    std::uint64_t addPreparedDocument(PreparedDocumentData&& prepared) {
        const std::uint32_t doc_id = static_cast<std::uint32_t>(documents_.size());
        documents_.push_back(Document{
            doc_id,
            pathToUtf8(prepared.path),
            prepared.path.filename().empty() ? pathToUtf8(prepared.path) : pathToUtf8(prepared.path.filename()),
            std::move(prepared.type)
        });

        std::uint64_t total_len = 0;
        for (PreparedFragmentData& prepared_fragment : prepared.fragments) {
            const std::uint32_t fragment_id = static_cast<std::uint32_t>(fragments_.size());
            fragments_.push_back(Fragment{
                fragment_id,
                doc_id,
                prepared_fragment.ordinal,
                std::move(prepared_fragment.text),
                prepared_fragment.length,
                prepared_fragment.metadata_only
            });
            total_len += prepared_fragment.length;
            fragment_terms_.push_back(std::move(prepared_fragment.unique_terms));
            for (const auto& [term, tf] : prepared_fragment.term_counts) {
                postings_[term].push_back(Posting{fragment_id, tf});
                vocabulary_.insert(term);
            }
        }
        return total_len;
    }

    template <typename T>
    static void writeNumber(std::ofstream& out, T value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template <typename T>
    static bool readNumber(std::ifstream& in, T& value) {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return static_cast<bool>(in);
    }

    static void writeString(std::ofstream& out, const std::string& value) {
        writeNumber<std::uint64_t>(out, value.size());
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
    }

    static bool readString(std::ifstream& in, std::string& value) {
        std::uint64_t size = 0;
        if (!readNumber<std::uint64_t>(in, size)) {
            return false;
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        value.assign(static_cast<std::size_t>(size), '\0');
        if (size > 0) {
            in.read(value.data(), static_cast<std::streamsize>(size));
        }
        return static_cast<bool>(in);
    }

    void rebuildDocumentAndSegmentIndices() {
        document_terms_.clear();
        doc_postings_.clear();
        segments_.clear();
        avg_doc_len_ = 1.0;

        if (documents_.empty()) {
            return;
        }

        document_terms_.assign(documents_.size(), {});
        std::uint64_t total_doc_len = 0;
        for (Document& doc : documents_) {
            doc.length = 0;
            doc.first_fragment_id = 0;
            doc.fragment_count = 0;
            doc.segment_id = 0;
        }

        for (const Fragment& fragment : fragments_) {
            Document& doc = documents_.at(fragment.doc_id);
            if (doc.fragment_count == 0) {
                doc.first_fragment_id = fragment.id;
            }
            ++doc.fragment_count;
            doc.length += fragment.length;
        }

        for (const auto& [term, postings] : postings_) {
            std::unordered_map<std::uint32_t, std::uint32_t> doc_counts;
            doc_counts.reserve(postings.size());
            for (const Posting& posting : postings) {
                const std::uint32_t doc_id = fragments_.at(posting.fragment_id).doc_id;
                doc_counts[doc_id] += posting.tf;
            }

            auto& doc_posting_list = doc_postings_[term];
            doc_posting_list.reserve(doc_counts.size());
            for (const auto& [doc_id, tf] : doc_counts) {
                doc_posting_list.push_back(DocPosting{doc_id, tf});
                document_terms_.at(doc_id).push_back(term);
            }
            std::sort(doc_posting_list.begin(), doc_posting_list.end(), [](const DocPosting& a, const DocPosting& b) {
                return a.doc_id < b.doc_id;
            });
        }

        for (std::vector<std::string>& terms : document_terms_) {
            std::sort(terms.begin(), terms.end());
            terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        }

        for (const Document& doc : documents_) {
            total_doc_len += doc.length;
        }
        avg_doc_len_ = documents_.empty() ? 1.0 : static_cast<double>(total_doc_len) / documents_.size();
        if (avg_doc_len_ <= 0.0) {
            avg_doc_len_ = 1.0;
        }

        constexpr std::size_t target_docs_per_segment = 64;
        constexpr std::size_t target_fragments_per_segment = 384;
        std::size_t doc_cursor = 0;
        while (doc_cursor < documents_.size()) {
            Segment segment;
            segment.id = static_cast<std::uint32_t>(segments_.size());
            segment.first_doc_id = static_cast<std::uint32_t>(doc_cursor);
            segment.first_fragment_id = documents_[doc_cursor].fragment_count == 0
                ? static_cast<std::uint32_t>(fragments_.size())
                : documents_[doc_cursor].first_fragment_id;

            std::size_t docs_in_segment = 0;
            std::size_t fragments_in_segment = 0;
            std::size_t segment_length = 0;
            while (doc_cursor < documents_.size()) {
                const Document& next_doc = documents_[doc_cursor];
                const bool hit_doc_limit = docs_in_segment >= target_docs_per_segment;
                const bool hit_fragment_limit =
                    docs_in_segment > 0 && fragments_in_segment + next_doc.fragment_count > target_fragments_per_segment;
                if (hit_doc_limit || hit_fragment_limit) {
                    break;
                }
                ++docs_in_segment;
                fragments_in_segment += next_doc.fragment_count;
                segment_length += next_doc.length;
                ++doc_cursor;
            }

            if (docs_in_segment == 0) {
                ++docs_in_segment;
                if (doc_cursor < documents_.size()) {
                    fragments_in_segment += documents_[doc_cursor].fragment_count;
                    segment_length += documents_[doc_cursor].length;
                    ++doc_cursor;
                }
            }

            segment.doc_count = static_cast<std::uint32_t>(docs_in_segment);
            segment.fragment_count = static_cast<std::uint32_t>(fragments_in_segment);
            segment.length = static_cast<std::uint32_t>(segment_length);
            segments_.push_back(segment);
        }

        for (Segment& segment : segments_) {
            for (std::uint32_t offset = 0; offset < segment.doc_count; ++offset) {
                documents_.at(segment.first_doc_id + offset).segment_id = segment.id;
            }
        }
    }

    void rebuildAuxiliaryIndices() {
        vocabulary_terms_.clear();
        ngram_index_.clear();
        vocabulary_terms_.reserve(vocabulary_.size());
        for (const std::string& term : vocabulary_) {
            vocabulary_terms_.push_back(term);
        }
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(vocabulary_terms_.size()); ++i) {
            for (const std::string& gram : ngramsForTerm(vocabulary_terms_[i])) {
                ngram_index_[gram].push_back(i);
            }
        }
    }

    std::unordered_map<std::string, double> expandQuery(const std::vector<std::string>& base_terms) const {
        std::unordered_map<std::string, double> weights;
        const auto& aliases = aliasMap();

        auto add = [&](const std::string& term, double weight) {
            if (term.empty()) {
                return;
            }
            auto it = weights.find(term);
            if (it == weights.end()) {
                weights.emplace(term, weight);
            } else {
                it->second = std::max(it->second, weight);
            }
        };

        for (const std::string& term : base_terms) {
            add(term, 1.0);
            const auto alias_it = aliases.find(term);
            if (alias_it != aliases.end()) {
                for (const std::string& alias : alias_it->second) {
                    add(alias, 0.62);
                }
            }
        }

        for (const std::string& term : base_terms) {
            const auto term_cps = utf8ToCodepoints(term);
            if (term_cps.size() < 4) {
                continue;
            }
            const int limit = term_cps.size() <= 6 ? 1 : 2;
            const auto term_grams = ngramsForTerm(term);
            std::unordered_map<std::uint32_t, std::uint16_t> overlaps;
            for (const std::string& gram : term_grams) {
                const auto gram_it = ngram_index_.find(gram);
                if (gram_it == ngram_index_.end()) {
                    continue;
                }
                for (const std::uint32_t candidate_id : gram_it->second) {
                    ++overlaps[candidate_id];
                }
            }

            std::vector<std::pair<std::uint32_t, std::uint16_t>> candidates;
            candidates.reserve(overlaps.size());
            for (const auto& [candidate_id, overlap] : overlaps) {
                if (overlap != 0) {
                    candidates.emplace_back(candidate_id, overlap);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) {
                    return a.second > b.second;
                }
                return a.first < b.first;
            });
            if (candidates.size() > 256) {
                candidates.resize(256);
            }

            int added = 0;
            for (const auto& [candidate_id, overlap] : candidates) {
                const std::string& candidate = vocabulary_terms_[candidate_id];
                if (weights.count(candidate) != 0) {
                    continue;
                }
                const auto candidate_cps = utf8ToCodepoints(candidate);
                if (candidate_cps.empty() ||
                    std::abs(static_cast<int>(candidate_cps.size()) - static_cast<int>(term_cps.size())) > limit) {
                    continue;
                }
                if (term_cps.front() != candidate_cps.front() && term_cps.back() != candidate_cps.back()) {
                    continue;
                }
                if (term_grams.size() >= 3 && overlap < 2) {
                    continue;
                }
                const int distance = boundedLevenshtein(term_cps, candidate_cps, limit);
                if (distance <= limit) {
                    add(candidate, distance == 1 ? 0.50 : 0.36);
                    if (++added >= 10) {
                        break;
                    }
                }
            }
        }

        return weights;
    }

    std::vector<Document> documents_;
    std::vector<Fragment> fragments_;
    std::vector<Segment> segments_;
    std::vector<std::vector<std::string>> document_terms_;
    std::vector<std::vector<std::string>> fragment_terms_;
    std::unordered_map<std::string, std::vector<DocPosting>> doc_postings_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::unordered_set<std::string> vocabulary_;
    std::vector<std::string> vocabulary_terms_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> ngram_index_;
    double avg_fragment_len_ = 1.0;
    double avg_doc_len_ = 1.0;
};

std::size_t safeSnippetStart(const std::string& text, std::size_t pos, std::size_t window) {
    std::size_t start = pos > window ? pos - window : 0;
    while (start < text.size() && isContinuationByte(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return start;
}

std::size_t safeSnippetEnd(const std::string& text, std::size_t pos, std::size_t window) {
    std::size_t end = std::min(text.size(), pos + window);
    while (end > 0 && end < text.size() && isContinuationByte(static_cast<unsigned char>(text[end]))) {
        --end;
    }
    return end;
}

std::string highlightText(const std::string& text,
                          const std::unordered_set<std::string>& terms,
                          bool color) {
    if (!color || terms.empty()) {
        return text;
    }
    const std::string begin = "\033[1;33m";
    const std::string end = "\033[0m";
    std::string out;
    std::size_t cursor = 0;
    for (const TokenSpan& token : tokenizeWithSpans(text)) {
        if (terms.count(token.norm) == 0) {
            continue;
        }
        out.append(text, cursor, token.begin - cursor);
        out += begin;
        out.append(text, token.begin, token.end - token.begin);
        out += end;
        cursor = token.end;
    }
    out.append(text, cursor, std::string::npos);
    return out;
}

std::string makeSnippet(const std::string& text,
                        const std::unordered_set<std::string>& terms,
                        bool color) {
    std::size_t match_pos = std::string::npos;
    for (const TokenSpan& token : tokenizeWithSpans(text)) {
        if (terms.count(token.norm) != 0) {
            match_pos = token.begin;
            break;
        }
    }
    if (match_pos == std::string::npos) {
        match_pos = std::min<std::size_t>(text.size(), 180);
    }

    const std::size_t start = safeSnippetStart(text, match_pos, 220);
    const std::size_t end = safeSnippetEnd(text, match_pos, 420);
    std::string snippet = text.substr(start, end - start);
    replaceAll(snippet, "\n", " ");
    snippet = trim(snippet);
    if (start > 0) {
        snippet = "..." + snippet;
    }
    if (end < text.size()) {
        snippet += "...";
    }
    return highlightText(snippet, terms, color);
}

double sentenceOverlapScore(const std::string& sentence, const std::vector<std::string>& query_terms) {
    if (query_terms.empty()) {
        return 0.0;
    }
    std::unordered_set<std::string> sentence_terms;
    for (const TokenSpan& token : tokenizeWithSpans(sentence)) {
        sentence_terms.insert(token.norm);
    }
    double score = 0.0;
    for (const std::string& term : query_terms) {
        if (sentence_terms.count(term) != 0) {
            score += 1.0;
        }
    }
    return score / static_cast<double>(query_terms.size());
}

std::string bestSentenceForAnswer(const std::string& text, const std::vector<std::string>& query_terms) {
    const auto sentences = splitSentences(text);
    if (sentences.empty()) {
        return trim(text).substr(0, 450);
    }

    double best_score = -1.0;
    std::string best;
    for (const std::string& sentence : sentences) {
        const double score = sentenceOverlapScore(sentence, query_terms);
        if (score > best_score) {
            best_score = score;
            best = sentence;
        }
    }
    if (best.size() > 520) {
        best = best.substr(0, 520) + "...";
    }
    return best;
}

std::string fragmentAnchor(const Fragment& fragment) {
    if (fragment.metadata_only) {
        return "#filename";
    }
    return "#fragment-" + std::to_string(fragment.ordinal);
}

void printResults(const SearchIndex& index,
                  const std::string& query,
                  const std::vector<SearchResult>& results,
                  bool color) {
    const auto terms = index.highlightTerms(query);
    if (results.empty()) {
        std::cout << "Ничего не найдено.\n";
        return;
    }

    for (std::size_t i = 0; i < results.size(); ++i) {
        const Fragment& fragment = index.fragment(results[i].fragment_id);
        const Document& doc = index.document(fragment.doc_id);
        std::cout << "\n[" << (i + 1) << "] score=" << std::fixed << std::setprecision(3)
                  << results[i].score << "  " << doc.path << fragmentAnchor(fragment) << "\n";
        std::cout << makeSnippet(fragment.text, terms, color) << "\n";
    }
}

void printExtractiveAnswer(const SearchIndex& index,
                           const std::string& query,
                           const std::vector<SearchResult>& results,
                           bool color) {
    if (results.empty()) {
        return;
    }

    const auto query_terms = queryBaseTerms(query);
    const auto terms = index.highlightTerms(query);
    std::cout << "\nОтвет по найденным локальным фрагментам:\n";

    const std::size_t answer_count = std::min<std::size_t>(results.size(), 5);
    for (std::size_t i = 0; i < answer_count; ++i) {
        const Fragment& fragment = index.fragment(results[i].fragment_id);
        const std::string sentence = bestSentenceForAnswer(fragment.text, query_terms);
        std::cout << "- " << highlightText(sentence, terms, color) << " [" << (i + 1) << "]\n";
    }

    std::cout << "\nИсточники:\n";
    for (std::size_t i = 0; i < answer_count; ++i) {
        const Fragment& fragment = index.fragment(results[i].fragment_id);
        const Document& doc = index.document(fragment.doc_id);
        std::cout << "[" << (i + 1) << "] " << doc.path << fragmentAnchor(fragment) << "\n";
    }
}

void printUsage() {
    std::cout
        << "LocalDocSearch - локальный поиск по TXT, Markdown, DOCX и PDF\n\n"
        << "Команды:\n"
        << "  docsearch build <folder> [-o index.bin] [-j threads]\n"
        << "  docsearch search <index.bin> <query words...> [-n 8] [--answer] [--no-color]\n"
        << "  docsearch interactive <index.bin> [-n 8] [--answer] [--no-color]\n"
        << "  docsearch query <folder> <query words...> [-n 8] [--answer] [--no-color] [-j threads]\n\n"
        << "Примеры:\n"
        << "  docsearch build ./docs -o docs.idx -j 6\n"
        << "  docsearch search docs.idx кто отвечает за оплату --answer\n"
        << "  docsearch query ./docs семантический поиск по PDF -n 5\n";
}

struct SearchOptions {
    std::size_t limit = 8;
    std::size_t threads = 0;
    bool answer = false;
    bool color = true;
    std::string query;
};

SearchOptions parseSearchOptions(const std::vector<std::string>& args, int start) {
    SearchOptions options;
    std::vector<std::string> query_parts;
    for (int i = start; i < static_cast<int>(args.size()); ++i) {
        const std::string& arg = args[static_cast<std::size_t>(i)];
        if ((arg == "-n" || arg == "--limit") && i + 1 < static_cast<int>(args.size())) {
            options.limit = static_cast<std::size_t>(std::max(1, std::atoi(args[static_cast<std::size_t>(++i)].c_str())));
        } else if ((arg == "-j" || arg == "--threads") && i + 1 < static_cast<int>(args.size())) {
            options.threads = static_cast<std::size_t>(std::max(1, std::atoi(args[static_cast<std::size_t>(++i)].c_str())));
        } else if (arg == "--answer") {
            options.answer = true;
        } else if (arg == "--no-color") {
            options.color = false;
        } else {
            query_parts.push_back(arg);
        }
    }
    std::ostringstream q;
    for (std::size_t i = 0; i < query_parts.size(); ++i) {
        if (i != 0) {
            q << ' ';
        }
        q << query_parts[i];
    }
    options.query = q.str();
    return options;
}

bool commandBuild(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        printUsage();
        return false;
    }
    fs::path folder = pathFromUtf8(args[2]);
    fs::path output = "docsearch.idx";
    std::size_t threads = 0;
    for (int i = 3; i < static_cast<int>(args.size()); ++i) {
        const std::string& arg = args[static_cast<std::size_t>(i)];
        if ((arg == "-o" || arg == "--output") && i + 1 < static_cast<int>(args.size())) {
            output = pathFromUtf8(args[static_cast<std::size_t>(++i)]);
        } else if ((arg == "-j" || arg == "--threads") && i + 1 < static_cast<int>(args.size())) {
            threads = static_cast<std::size_t>(std::max(1, std::atoi(args[static_cast<std::size_t>(++i)].c_str())));
        }
    }

    SearchIndex index;
    if (!index.buildFromFolder(folder, threads)) {
        std::cerr << "Index is empty. Check document folder and supported formats.\n";
        return false;
    }
    if (!index.save(output)) {
        std::cerr << "Cannot save index: " << pathToUtf8(output) << "\n";
        return false;
    }
    std::cout << "Индекс сохранен: " << pathToUtf8(output) << "\n"
              << "Документов: " << index.documentCount() << ", фрагментов: "
              << index.fragmentCount() << ", сегментов: "
              << index.segmentCount() << "\n";
    return true;
}

bool commandSearch(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        printUsage();
        return false;
    }
    const fs::path index_path = pathFromUtf8(args[2]);
    SearchOptions options = parseSearchOptions(args, 3);
    if (trim(options.query).empty()) {
        std::cerr << "Empty query.\n";
        return false;
    }

    SearchIndex index;
    if (!index.load(index_path)) {
        std::cerr << "Cannot load index: " << pathToUtf8(index_path) << "\n";
        return false;
    }
    const auto results = index.search(options.query, options.limit);
    printResults(index, options.query, results, options.color);
    if (options.answer) {
        printExtractiveAnswer(index, options.query, results, options.color);
    }
    return true;
}

bool commandQuery(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        printUsage();
        return false;
    }
    const fs::path folder = pathFromUtf8(args[2]);
    SearchOptions options = parseSearchOptions(args, 3);
    if (trim(options.query).empty()) {
        std::cerr << "Empty query.\n";
        return false;
    }

    SearchIndex index;
    if (!index.buildFromFolder(folder, options.threads)) {
        std::cerr << "Index is empty. Check document folder and supported formats.\n";
        return false;
    }
    const auto results = index.search(options.query, options.limit);
    printResults(index, options.query, results, options.color);
    if (options.answer) {
        printExtractiveAnswer(index, options.query, results, options.color);
    }
    return true;
}

bool commandInteractive(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        printUsage();
        return false;
    }
    const fs::path index_path = pathFromUtf8(args[2]);
    SearchOptions options = parseSearchOptions(args, 3);

    SearchIndex index;
    if (!index.load(index_path)) {
        std::cerr << "Cannot load index: " << pathToUtf8(index_path) << "\n";
        return false;
    }

    std::cout << "Loaded " << index.documentCount() << " documents, " << index.fragmentCount()
              << " fragments. Type query, or :q to exit.\n";
    std::string query;
    while (true) {
        std::cout << "\nsearch> " << std::flush;
        if (!std::getline(std::cin, query)) {
            break;
        }
        query = trim(query);
        if (query == ":q" || query == ":quit" || query == "exit") {
            break;
        }
        if (query.empty()) {
            continue;
        }
        const auto results = index.search(query, options.limit);
        printResults(index, query, results, options.color);
        if (options.answer) {
            printExtractiveAnswer(index, query, results, options.color);
        }
    }
    return true;
}

void configureConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

std::vector<std::string> commandLineArgs(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv != nullptr) {
        args.reserve(static_cast<std::size_t>(wide_argc));
        for (int i = 0; i < wide_argc; ++i) {
            args.push_back(wideToUtf8(wide_argv[i]));
        }
        LocalFree(wide_argv);
        return args;
    }
#endif
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    configureConsole();
    std::ios::sync_with_stdio(false);

    const std::vector<std::string> args = commandLineArgs(argc, argv);
    if (args.size() < 2) {
        printUsage();
        return 0;
    }

    const std::string command = args[1];
    try {
        if (command == "build") {
            return commandBuild(args) ? 0 : 1;
        }
        if (command == "search") {
            return commandSearch(args) ? 0 : 1;
        }
        if (command == "interactive") {
            return commandInteractive(args) ? 0 : 1;
        }
        if (command == "query") {
            return commandQuery(args) ? 0 : 1;
        }
        if (command == "-h" || command == "--help" || command == "help") {
            printUsage();
            return 0;
        }
        std::cerr << "Unknown command: " << command << "\n";
        printUsage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 2;
    }
}
