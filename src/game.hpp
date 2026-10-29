#pragma once
#include <chrono>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "settings.hpp"

namespace xonix {

enum class Cell : std::uint8_t { Empty, Claimed, Trail };
enum class Dir : std::uint8_t { None, Up, Down, Left, Right };
enum class Phase : std::uint8_t { Playing, LevelClear, GameOver };

struct Point {
    int x, y;
    friend bool operator==(Point, Point) = default;
};
struct Enemy {
    Point pos, vel;  // vel components are +-1
};

// Contiguous, row-major.
class Grid {
public:
    explicit Grid(Size s) : size_(s), cells_(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height)) {}
    Cell& operator[](int x, int y) { return cells_[at(x, y)]; }
    Cell operator[](int x, int y) const { return cells_[at(x, y)]; }
    Cell& operator[](Point p) { return cells_[at(p.x, p.y)]; }
    Cell operator[](Point p) const { return cells_[at(p.x, p.y)]; }
    Size size() const { return size_; }
    bool contains(Point p) const { return p.x >= 0 && p.y >= 0 && p.x < size_.width && p.y < size_.height; }
    int index(Point p) const { return p.y * size_.width + p.x; }
    std::span<Cell> cells() { return cells_; }

private:
    std::size_t at(int x, int y) const { return static_cast<std::size_t>(y * size_.width + x); }
    Size size_;
    std::vector<Cell> cells_;
};

// Plain state, public for the UI and tests.
class Game {
public:
    Game(Size board, std::string name, std::uint32_t seed);
    void steer(Dir d);     // ignored when a trail exists and d reverses last_step
    void tick();           // one fixed step; no-op unless phase == Playing
    void next_level();     // LevelClear -> Playing, level+1, +1 life, fresh field
    void restart();        // score 0, level 1, start lives, same name, fresh field
    int percent() const;   // claimed interior * 100 / interior total
    Point spawn() const;   // {width / 2, 0}: middle of the top border

    std::string name;
    Grid grid;
    Point player;
    Dir dir = Dir::None, last_step = Dir::None;
    std::vector<Enemy> enemies;
    std::vector<Point> trail;
    int score = 0, lives, level = 1, claimed = 0, interior;
    int player_period, enemy_period;  // tests may change these
    Phase phase = Phase::Playing;

private:
    void reset_field();
    void move_player();
    void move_enemies();
    void capture();
    void lose_life();
    std::mt19937 rng_;
    int player_clock_ = 0, enemy_clock_ = 0;
    std::vector<std::uint8_t> mark_;
    std::vector<int> stack_;  // reused by capture()
};

class FixedStep {
public:
    explicit FixedStep(std::chrono::steady_clock::duration step, int max_steps) : step_(step), max_steps_(max_steps) {}
    void reset(std::chrono::steady_clock::time_point now) { last_ = now; acc_ = {}; }
    // Steps to run; a stall drops the excess.
    int advance(std::chrono::steady_clock::time_point now);

private:
    std::chrono::steady_clock::duration step_, acc_{};
    int max_steps_;
    std::chrono::steady_clock::time_point last_{};
};

}  // namespace xonix
