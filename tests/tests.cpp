#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "game.hpp"
#include "settings.hpp"

namespace {

int failures = 0;

}  // namespace

#define CHECK(...)                                                      \
    do {                                                                  \
        if (!(__VA_ARGS__)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #__VA_ARGS__ "\n"; \
            ++failures;                                                   \
        }                                                                 \
    } while (false)

namespace {

using xonix::Size;

std::expected<xonix::Options, std::string> parse(std::vector<std::string_view> args) {
    return xonix::parse_options(args);
}

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

std::size_t code_points(std::string_view s) {
    std::size_t n = 0;
    for (char ch : s)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    return n;
}

bool mentions(const std::string& msg, std::string_view option, std::string_view range) {
    return msg.find(option) != std::string::npos && msg.find(range) != std::string::npos;
}

void test_options() {
    auto d = parse({});
    CHECK(d.has_value() && d->board == Size{60, 20} && !d->help);

    auto both = parse({"--width", "80", "--height", "30"});
    CHECK(both.has_value() && both->board == Size{80, 30});

    auto eq = parse({"--width=80"});
    CHECK(eq.has_value() && eq->board == Size{80, 20});

    auto w19 = parse({"--width", "19"});
    CHECK(!w19.has_value() && mentions(w19.error(), "--width", "20") && mentions(w19.error(), "--width", "200"));
    CHECK(!parse({"--width", "201"}).has_value());
    auto h9 = parse({"--height", "9"});
    CHECK(!h9.has_value() && mentions(h9.error(), "--height", "10") && mentions(h9.error(), "--height", "80"));
    CHECK(!parse({"--height", "81"}).has_value());

    auto w20 = parse({"--width", "20"});
    CHECK(w20.has_value() && w20->board.width == 20);
    auto w200 = parse({"--width", "200"});
    CHECK(w200.has_value() && w200->board.width == 200);
    auto h10 = parse({"--height", "10"});
    CHECK(h10.has_value() && h10->board.height == 10);
    auto h80 = parse({"--height", "80"});
    CHECK(h80.has_value() && h80->board.height == 80);

    CHECK(!parse({"--width", "abc"}).has_value());
    CHECK(!parse({"--width", "12x"}).has_value());
    auto missing = parse({"--width"});
    CHECK(!missing.has_value() && missing.error().find("--width") != std::string::npos);

    CHECK(!parse({"--bogus"}).has_value());

    auto help = parse({"--help"});
    CHECK(help.has_value() && help->help);
}

void test_clean_name() {
    CHECK(xonix::clean_name("  Ann  ") == "Ann");
    CHECK(xonix::clean_name("") == "Player");
    CHECK(xonix::clean_name("   ") == "Player");

    std::string cyr;
    for (int i = 0; i < 20; ++i) cyr += "\xD0\x96";  // U+0416
    auto cut = xonix::clean_name(cyr);
    CHECK(code_points(cut) == 16);
    CHECK(is_valid_utf8(cut));
}

using namespace xonix;

void run(Game& g, int n) { for (int i = 0; i < n; ++i) g.tick(); }
void moves(Game& g, int k) { run(g, k * g.player_period); }
constexpr int frozen = 1'000'000;

Game make() {
    Game g({30, 14}, "T", 1);
    g.enemies.clear();
    return g;
}

void test_new_field() {
    Game g({30, 14}, "T", 1);
    bool border_ok = true, interior_ok = true;
    for (int y = 0; y < 14; ++y)
        for (int x = 0; x < 30; ++x) {
            bool inner = x >= 2 && x <= 27 && y >= 2 && y <= 11;
            (inner ? interior_ok : border_ok) &= (g.grid[x, y] == (inner ? Cell::Empty : Cell::Claimed));
        }
    CHECK(border_ok && interior_ok);
    CHECK(g.percent() == 0 && g.lives == 3 && g.interior == 260);
    CHECK(g.enemies.size() == 2);
    for (const auto& e : g.enemies) CHECK(g.grid[e.pos] == Cell::Empty);
    CHECK(g.spawn() == Point{15, 0} && g.player == Point{15, 0});
}

void test_full_capture() {
    Game g = make();
    g.steer(Dir::Down);
    moves(g, 12);
    bool all = true;
    for (int y = 2; y <= 11; ++y)
        for (int x = 2; x <= 27; ++x) all &= g.grid[x, y] == Cell::Claimed;
    CHECK(all);
    CHECK(g.score == 260 && g.phase == Phase::LevelClear && g.trail.empty() && g.dir == Dir::None);
}

void test_enemy_region_stays() {
    Game g = make();
    g.enemies.push_back({{5, 6}, {1, 1}});
    g.enemy_period = frozen;
    g.steer(Dir::Down);
    moves(g, 12);
    CHECK(g.claimed == 130 && g.score == 130 && g.percent() == 50 && g.phase == Phase::Playing);
    CHECK(g.grid[5, 6] == Cell::Empty && g.grid[2, 2] == Cell::Empty && g.grid[14, 11] == Cell::Empty);
    CHECK(g.grid[16, 2] == Cell::Claimed && g.grid[27, 11] == Cell::Claimed && g.grid[15, 5] == Cell::Claimed);
}

void test_enemy_hits_trail() {
    Game g = make();
    g.steer(Dir::Down);
    moves(g, 3);
    CHECK(g.trail.size() == 2);
    g.enemies.push_back({{16, 3}, {-1, -1}});
    g.enemy_period = 1;
    g.tick();
    CHECK(g.lives == 2 && g.grid[15, 2] == Cell::Empty && g.grid[15, 3] == Cell::Empty);
    CHECK(g.player == g.spawn() && g.claimed == 0 && g.trail.empty());
}

void test_crossing_trail() {
    Game g = make();
    g.steer(Dir::Down);
    moves(g, 3);
    g.steer(Dir::Right);
    moves(g, 1);
    g.steer(Dir::Up);
    moves(g, 1);
    g.steer(Dir::Left);
    moves(g, 1);
    CHECK(g.lives == 2 && g.trail.empty() && g.grid[15, 2] == Cell::Empty && g.grid[16, 2] == Cell::Empty);
}

void test_reversal_ignored() {
    Game g = make();
    g.steer(Dir::Down);
    moves(g, 3);
    g.steer(Dir::Up);
    CHECK(g.dir == Dir::Down);
    g.steer(Dir::Left);
    g.steer(Dir::Up);
    CHECK(g.dir == Dir::Left);
}

void test_player_walks_into_enemy() {
    Game g = make();
    g.enemies.push_back({{15, 3}, {1, 1}});
    g.enemy_period = frozen;
    g.steer(Dir::Down);
    moves(g, 3);
    CHECK(g.lives == 2 && g.player == g.spawn());
}

void test_bounce() {
    Game g = make();
    g.enemy_period = 1;
    g.enemies.push_back({{2, 5}, {-1, 1}});
    g.tick();
    CHECK(g.enemies[0].pos == Point{3, 6} && g.enemies[0].vel == Point{1, 1});

    Game c = make();
    c.enemy_period = 1;
    c.enemies.push_back({{2, 2}, {-1, -1}});
    c.tick();
    CHECK(c.enemies[0].pos == Point{3, 3} && c.enemies[0].vel == Point{1, 1});

    Game s = make();
    s.enemy_period = 1;
    for (Point p : {Point{6, 5}, Point{5, 6}, Point{4, 5}, Point{5, 4}}) s.grid[p] = Cell::Claimed;
    s.enemies.push_back({{5, 5}, {1, 1}});
    s.tick();
    CHECK(s.enemies[0].pos == Point{5, 5});
}

void test_level_transition() {
    Game g = make();
    g.steer(Dir::Down);
    moves(g, 12);
    g.next_level();
    CHECK(g.level == 2 && g.score == 260 && g.name == "T" && g.lives == 4 && g.percent() == 0);
    CHECK(g.enemies.size() == 3 && g.enemy_period == 9 && g.phase == Phase::Playing);
}

void test_game_over_restart() {
    Game g = make();
    g.lives = 1;
    g.enemies.push_back({{15, 2}, {1, 1}});  // first interior cell below the spawn
    g.enemy_period = frozen;
    g.steer(Dir::Down);
    moves(g, 2);
    CHECK(g.lives == 0 && g.phase == Phase::GameOver);
    g.steer(Dir::Down);
    Point p = g.player;
    run(g, 100);
    CHECK(g.player == p && g.phase == Phase::GameOver);
    g.restart();
    CHECK(g.score == 0 && g.level == 1 && g.lives == 3 && g.name == "T" && g.phase == Phase::Playing);
}

// Claims the first k cells of the left half (x 2..14) row by row, then closes column 15:
// the trail (10) and the enemy-free right half (120) add 130 more.
Game after_capture(int k) {
    Game g = make();
    g.enemies.push_back({{5, 10}, {1, 1}});  // keeps the rest of the left half open
    g.enemy_period = frozen;
    for (int i = 0; i < k; ++i) g.grid[2 + i % 13, 2 + i / 13] = Cell::Claimed;
    g.claimed = k;
    g.steer(Dir::Down);
    moves(g, 12);
    return g;
}

void test_capture_threshold() {
    const Game below = after_capture(64);  // 194 of 260 = 74%
    CHECK(below.claimed == 194 && below.percent() == 74 && below.phase == Phase::Playing);
    const Game at = after_capture(65);  // 195 of 260 = 75%
    CHECK(at.claimed == 195 && at.percent() == 75 && at.phase == Phase::LevelClear);
}

void test_enemy_spawns_distinct() {
    // 12 enemies in the 96 interior cells of the smallest board collide often by chance.
    bool full = true, distinct = true;
    for (std::uint32_t seed = 1; seed <= 50; ++seed) {
        Game g({20, 10}, "T", seed);
        g.level = rules.max_enemies;
        g.next_level();
        full &= g.enemies.size() == static_cast<std::size_t>(rules.max_enemies);
        for (std::size_t i = 0; i < g.enemies.size(); ++i)
            for (std::size_t j = i + 1; j < g.enemies.size(); ++j) distinct &= g.enemies[i].pos != g.enemies[j].pos;
    }
    CHECK(full && distinct);
}

void test_fixed_step() {
    using namespace std::chrono;
    FixedStep f(milliseconds(10), 10);
    auto t = steady_clock::now();
    f.reset(t);
    CHECK(f.advance(t + milliseconds(35)) == 3);
    CHECK(f.advance(t + milliseconds(40)) == 1);
    CHECK(f.advance(t + seconds(5)) == 10);
    CHECK(f.advance(t + seconds(5) + milliseconds(10)) == 1);
}
}  // namespace

int main() {
    test_options();
    test_clean_name();
    test_new_field();
    test_full_capture();
    test_enemy_region_stays();
    test_enemy_hits_trail();
    test_crossing_trail();
    test_reversal_ignored();
    test_player_walks_into_enemy();
    test_bounce();
    test_level_transition();
    test_game_over_restart();
    test_capture_threshold();
    test_enemy_spawns_distinct();
    test_fixed_step();
    if (failures != 0) std::cerr << failures << " check(s) failed\n";
    return failures;
}
