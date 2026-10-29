#pragma once
#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace xonix {

struct Size {
    int width, height;
    friend bool operator==(Size, Size) = default;
};

struct Rules {
    Size default_board{60, 20}, min_board{20, 10}, max_board{200, 80};
    int border = 2;
    int start_lives = 3, max_lives = 9, capture_percent = 75;
    int first_level_enemies = 2, max_enemies = 12;
    std::chrono::milliseconds step{10};
    int max_catch_up_steps = 10;
    int player_period = 6, enemy_period_start = 10, enemy_period_min = 4;
    std::size_t max_name_length = 16;  // code points
};
inline constexpr Rules rules{};

struct Options {
    Size board = rules.default_board;
    bool help = false;
};

// args excludes argv[0]
std::expected<Options, std::string> parse_options(std::span<const std::string_view> args);
// trim, cut to max_name_length code points, "" -> "Player"
std::string clean_name(std::string_view raw);
extern const std::string_view usage_text;

}  // namespace xonix
