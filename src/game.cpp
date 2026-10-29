#include "game.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace xonix {

namespace {

bool reverses(Dir a, Dir b) {
    return (a == Dir::Up && b == Dir::Down) || (a == Dir::Down && b == Dir::Up) ||
           (a == Dir::Left && b == Dir::Right) || (a == Dir::Right && b == Dir::Left);
}

Point step_of(Dir d) {
    switch (d) {
        case Dir::Up: return {0, -1};
        case Dir::Down: return {0, 1};
        case Dir::Left: return {-1, 0};
        case Dir::Right: return {1, 0};
        case Dir::None: break;
    }
    return {0, 0};
}

}  // namespace

Game::Game(Size board, std::string name_, std::uint32_t seed)
    : name(std::move(name_)), grid(board), lives(rules.start_lives), player_period(rules.player_period), rng_(seed) {
    reset_field();  // sets player, interior, enemy_period and the enemies
}

Point Game::spawn() const { return {grid.size().width / 2, 0}; }

int Game::percent() const { return claimed * 100 / interior; }

void Game::steer(Dir d) {
    if (!trail.empty() && reverses(d, last_step)) return;
    dir = d;
}

void Game::reset_field() {
    const auto [w, h] = grid.size();
    const int b = rules.border;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grid[x, y] = (x < b || y < b || x >= w - b || y >= h - b) ? Cell::Claimed : Cell::Empty;
    interior = (w - 2 * b) * (h - 2 * b);
    assert(interior > 0);  // parse_options keeps the board at least rules.min_board
    claimed = 0;
    trail.clear();
    trail.reserve(static_cast<std::size_t>(interior));
    stack_.clear();
    stack_.reserve(static_cast<std::size_t>(interior));
    mark_.clear();
    mark_.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));  // indexed by grid index
    player = spawn();
    dir = last_step = Dir::None;
    player_clock_ = enemy_clock_ = 0;
    enemy_period = std::max(rules.enemy_period_min, rules.enemy_period_start - (level - 1));

    const int count = std::min(rules.max_enemies, rules.first_level_enemies + level - 1);
    enemies.clear();
    enemies.reserve(static_cast<std::size_t>(rules.max_enemies));
    std::uniform_int_distribution<int> px(b, w - b - 1), py(b, h - b - 1), sign(0, 1);
    for (int i = 0; i < count; ++i) {
        Point p;
        do p = {px(rng_), py(rng_)};  // re-roll a taken cell, so enemies never start merged
        while (std::ranges::any_of(enemies, [&](const Enemy& e) { return e.pos == p; }));
        enemies.push_back({p, {sign(rng_) * 2 - 1, sign(rng_) * 2 - 1}});
    }
}

void Game::tick() {
    if (phase != Phase::Playing) return;
    if (++player_clock_ >= player_period) {
        player_clock_ = 0;
        move_player();
    }
    if (phase == Phase::Playing && ++enemy_clock_ >= enemy_period) {
        enemy_clock_ = 0;
        move_enemies();
    }
}

void Game::move_player() {
    if (dir == Dir::None) return;
    const Point d = step_of(dir);
    const Point next{player.x + d.x, player.y + d.y};
    if (!grid.contains(next)) {
        dir = Dir::None;
        return;
    }
    bool captured = false;
    switch (grid[next]) {
        case Cell::Trail:
            lose_life();
            return;
        case Cell::Empty:
            grid[next] = Cell::Trail;
            trail.push_back(next);
            break;
        case Cell::Claimed:
            captured = !trail.empty();
            break;
    }
    player = next;
    last_step = dir;
    if (captured) {
        capture();
        dir = Dir::None;
    }
    if (std::ranges::any_of(enemies, [&](const Enemy& e) { return e.pos == player; })) lose_life();
}

void Game::move_enemies() {
    const auto blocked = [&](int x, int y) { return !grid.contains({x, y}) || grid[x, y] == Cell::Claimed; };
    for (auto& e : enemies) {
        const auto [x, y] = e.pos;
        int vx = e.vel.x, vy = e.vel.y;
        const bool rx = blocked(x + vx, y), ry = blocked(x, y + vy);
        if (rx) vx = -vx;
        if (ry) vy = -vy;
        if (!rx && !ry && blocked(x + vx, y + vy)) vx = -vx, vy = -vy;
        e.vel = {vx, vy};
        // Stay put rather than squeeze diagonally between two claimed cells.
        if (blocked(x + vx, y + vy) || (blocked(x + vx, y) && blocked(x, y + vy))) continue;
        const Point n{x + vx, y + vy};
        if (grid[n] == Cell::Trail || n == player) {
            lose_life();
            return;
        }
        e.pos = n;
    }
}

void Game::capture() {
    int newly = 0;
    for (Point p : trail) grid[p] = Cell::Claimed;
    newly += static_cast<int>(trail.size());
    trail.clear();

    mark_.assign(static_cast<std::size_t>(grid.size().width) * static_cast<std::size_t>(grid.size().height), 0);
    stack_.clear();
    const auto visit = [&](Point p) {
        if (!grid.contains(p) || grid[p] != Cell::Empty) return;
        auto& m = mark_[static_cast<std::size_t>(grid.index(p))];
        if (m) return;
        m = 1;  // marked on push: the stack never exceeds the interior size
        stack_.push_back(grid.index(p));
    };
    for (const Enemy& e : enemies) visit(e.pos);
    const int w = grid.size().width;
    while (!stack_.empty()) {
        const int i = stack_.back();
        stack_.pop_back();
        const Point p{i % w, i / w};
        visit({p.x + 1, p.y});
        visit({p.x - 1, p.y});
        visit({p.x, p.y + 1});
        visit({p.x, p.y - 1});
    }
    std::size_t i = 0;
    for (Cell& c : grid.cells()) {
        if (c == Cell::Empty && !mark_[i]) {
            c = Cell::Claimed;
            ++newly;
        }
        ++i;
    }
    claimed += newly;
    score += newly * level;
    if (percent() >= rules.capture_percent) phase = Phase::LevelClear;
}

void Game::lose_life() {
    for (Point p : trail) grid[p] = Cell::Empty;
    trail.clear();
    player = spawn();
    dir = last_step = Dir::None;
    if (--lives == 0) phase = Phase::GameOver;
}

void Game::next_level() {
    ++level;
    lives = std::min(rules.max_lives, lives + 1);
    phase = Phase::Playing;
    reset_field();
}

void Game::restart() {
    score = 0;
    level = 1;
    lives = rules.start_lives;
    phase = Phase::Playing;
    reset_field();
}

int FixedStep::advance(std::chrono::steady_clock::time_point now) {
    acc_ += now - last_;
    last_ = now;
    auto n = acc_ / step_;
    acc_ -= n * step_;
    if (n > max_steps_) {
        n = max_steps_;
        acc_ = {};
    }
    return static_cast<int>(n);
}

}  // namespace xonix
