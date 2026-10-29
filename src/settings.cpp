#include "settings.hpp"

#include <charconv>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

namespace xonix {

const std::string_view usage_text =
    "Usage: xonixm23 [--width N] [--height N] [--help]\n"
    "  --width N    board width in cells, 20-200 (default 60)\n"
    "  --height N   board height in cells, 10-80 (default 20)\n"
    "  --help       show this help and exit\n";

namespace {

std::expected<int, std::string> parse_int(std::string_view name, std::string_view text,
                                          int lo, int hi) {
    int value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (text.empty() || ec != std::errc{} || ptr != last || value < lo || value > hi)
        return std::unexpected(
            std::format("{} must be an integer from {} to {}, got '{}'", name, lo, hi, text));
    return value;
}

}  // namespace

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    // false: args[i] is not `name`; true: stored in `field` (i moves past a separate value).
    const auto try_value = [&](std::size_t& i, std::string_view name, int lo, int hi,
                               int& field) -> std::expected<bool, std::string> {
        const std::string_view arg = args[i];
        std::string_view value;
        if (arg == name) {
            if (i + 1 >= args.size()) return std::unexpected(std::format("{} requires a value", name));
            value = args[++i];
        } else if (arg.starts_with(name) && arg.substr(name.size()).starts_with('=')) {
            value = arg.substr(name.size() + 1);
        } else {
            return false;
        }
        auto n = parse_int(name, value, lo, hi);
        if (!n) return std::unexpected(std::move(n.error()));
        field = *n;
        return true;
    };

    Options opts;
    const Size lo = rules.min_board, hi = rules.max_board;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help") {
            opts.help = true;
            continue;
        }
        auto r = try_value(i, "--width", lo.width, hi.width, opts.board.width);
        if (r && !*r) r = try_value(i, "--height", lo.height, hi.height, opts.board.height);
        if (!r) return std::unexpected(std::move(r.error()));
        if (!*r) return std::unexpected(std::format("unknown argument '{}'", args[i]));
    }
    return opts;
}

std::string clean_name(std::string_view raw) {
    constexpr std::string_view ws = " \t\r\n";
    const auto b = raw.find_first_not_of(ws);
    if (b == std::string_view::npos) return "Player";
    const auto e = raw.find_last_not_of(ws);
    std::string_view s = raw.substr(b, e - b + 1);

    std::size_t points = 0, cut = s.size();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (points == rules.max_name_length) {
            cut = i;
            break;
        }
        ++points;
    }
    s = s.substr(0, cut);
    // trimming after truncation: the cut may leave trailing spaces
    const auto e2 = s.find_last_not_of(ws);
    return std::string(s.substr(0, e2 + 1));
}

}  // namespace xonix
