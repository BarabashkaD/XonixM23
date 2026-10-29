// XonixM23 front end: name entry, board, status and help lines, overlays,
// phase-gated keys, the too-small screen, and the wake-up ticker thread.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "game.hpp"
#include "settings.hpp"

// Only FTXUI is pulled in; every xonix name stays qualified (xonix::Cell vs ftxui::Cell).
using namespace ftxui;

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto frame_interval = std::chrono::milliseconds(16);
constexpr int overlay_width = 36;                                             // widest overlay, border included
constexpr std::string_view help_line = "Arrows/WASD move  P pause  Q quit";  // never wider than overlay_width

// Draws the field straight into the screen, so no per-cell elements are built each frame.
class Board final : public Node {
public:
    explicit Board(const xonix::Game& game) : game_(game) {}

    void ComputeRequirement() override {
        requirement_ = {};
        requirement_.min_x = game_.grid.size().width;
        requirement_.min_y = game_.grid.size().height;
    }

    void Render(Screen& screen) override {
        const xonix::Size s = game_.grid.size();
        clip_ = Box::Intersection(box_, screen.stencil);
        clip_.x_max = std::min(clip_.x_max, box_.x_min + s.width - 1);
        clip_.y_max = std::min(clip_.y_max, box_.y_min + s.height - 1);
        for (int y = clip_.y_min; y <= clip_.y_max; ++y) {
            for (int x = clip_.x_min; x <= clip_.x_max; ++x) {
                switch (game_.grid[x - box_.x_min, y - box_.y_min]) {
                    case xonix::Cell::Empty: put(screen, x, y, " ", Color::Default); break;
                    case xonix::Cell::Claimed: put(screen, x, y, "▒", Color::GrayDark); break;
                    case xonix::Cell::Trail: put(screen, x, y, "+", Color::Yellow); break;
                }
            }
        }
        for (const xonix::Enemy& e : game_.enemies) put_at(screen, e.pos, "o", Color::Red);
        put_at(screen, game_.player, "@", Color::White, true);
    }

private:
    static void put(Screen& screen, int x, int y, std::string_view glyph, Color fg, bool bold = false) {
        ftxui::Cell& cell = screen.CellAt(x, y);
        cell.character.assign(glyph);  // short glyphs stay in the string's inline buffer
        cell.foreground_color = fg;
        cell.bold = bold;
    }
    void put_at(Screen& screen, xonix::Point p, std::string_view glyph, Color fg, bool bold = false) const {
        const int x = box_.x_min + p.x, y = box_.y_min + p.y;
        if (clip_.Contain(x, y)) put(screen, x, y, glyph, fg, bold);
    }

    const xonix::Game& game_;
    Box clip_;
};

std::string status_line(const std::string& name, int score, int lives, int level, int percent) {
    return name + "  Score " + std::to_string(score) + "  Lives " + std::to_string(lives) + "  Level " +
           std::to_string(level) + "  Captured " + std::to_string(percent) + "% / " +
           std::to_string(xonix::rules.capture_percent) + "%";
}

std::string status_line(const xonix::Game& g) { return status_line(g.name, g.score, g.lives, g.level, g.percent()); }

// Width of the status line at worst-case values, reserved once per game so the
// required terminal size never changes mid-game (e.g. when the score gains a digit).
int reserved_status_width(const std::string& name) {
    return string_width(status_line(name, 9'999'999, xonix::rules.max_lives, 999, 100));
}

struct Fit {
    int need_w, need_h, have_w, have_h;
    bool ok() const { return have_w >= need_w && have_h >= need_h; }
};

Fit measure(const xonix::Game& g, int status_width) {
    const xonix::Size s = g.grid.size();
    const Dimensions term = Terminal::Size();
    return {std::max({s.width, status_width, overlay_width}), s.height + 2, term.dimx, term.dimy};
}

std::string dims(int w, int h) { return std::to_string(w) + "×" + std::to_string(h); }

// A bordered, centred box drawn over the board; the first line is the title.
Element dialog(const std::vector<std::string>& lines) {
    Elements rows;
    for (const std::string& line : lines) rows.push_back(text(" " + line + " ") | hcenter);
    rows.front() = rows.front() | bold;
    return vbox(std::move(rows)) | border | clear_under | center;
}

// Lower-case ASCII key of a single-character event, or 0.
char key_of(const Event& e) {
    if (!e.is_character() || e.input().size() != 1) return 0;
    return static_cast<char>(std::tolower(static_cast<unsigned char>(e.input()[0])));
}

xonix::Dir dir_of(const Event& e, char k) {
    if (e == Event::ArrowUp || k == 'w') return xonix::Dir::Up;
    if (e == Event::ArrowDown || k == 's') return xonix::Dir::Down;
    if (e == Event::ArrowLeft || k == 'a') return xonix::Dir::Left;
    if (e == Event::ArrowRight || k == 'd') return xonix::Dir::Right;
    return xonix::Dir::None;
}

// Owns the game state; every member is touched only on the UI thread.
class Ui {
public:
    Ui(App& app, xonix::Size board) : app_(app), board_(board) {
        InputOption opt;
        opt.content = &name_;
        opt.placeholder = "Player";
        opt.multiline = false;
        opt.on_enter = [this] { start(); };
        input_ = Input(opt);
    }
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    Component root() {
        return CatchEvent(Renderer(input_, [this] { return game_ ? game_view() : name_view(); }),
                          [this](const Event& e) { return on_event(e); });
    }

private:
    void start() {
        if (game_) return;  // a second queued Enter must not replace a running game
        game_.emplace(board_, xonix::clean_name(name_), static_cast<std::uint32_t>(std::random_device{}()));
        status_width_ = reserved_status_width(game_->name);
        resume(Clock::now());
    }

    Fit measure_now() const { return measure(*game_, status_width_); }

    // A terminal that is too small pauses a running game; nothing else changes.
    // Only the event path calls this: rendering never changes state.
    bool fits() {
        const bool ok = measure_now().ok();
        if (!ok && game_->phase == xonix::Phase::Playing) paused_ = true;
        return ok;
    }

    bool on_event(const Event& e) {
        const char k = key_of(e);
        if (!game_) {
            if (e == Event::Escape) app_.Exit();
            return e == Event::Escape || e == Event::Custom;  // everything else goes to the name input
        }
        if (e == Event::Escape || k == 'q') {
            app_.Exit();
            return true;
        }
        xonix::Game& g = *game_;
        const bool ok = fits();
        const auto now = Clock::now();
        if (e == Event::Custom) {
            if (ok && g.phase == xonix::Phase::Playing && !paused_) {
                for (int n = fixed_.advance(now); n > 0; --n) g.tick();
            } else {
                fixed_.reset(now);  // no catch-up after a pause, a resize or a dialog
            }
            return true;
        }
        if (!ok) return true;
        switch (g.phase) {
            case xonix::Phase::Playing:
                if (k == 'p' || k == ' ') {
                    paused_ = !paused_;
                    fixed_.reset(now);
                } else if (!paused_) {
                    if (const xonix::Dir d = dir_of(e, k); d != xonix::Dir::None) g.steer(d);
                }
                break;
            case xonix::Phase::LevelClear:
                if (e == Event::Return) {  // not Space: its key repeat would continue and then pause
                    g.next_level();
                    resume(now);
                }
                break;
            case xonix::Phase::GameOver:
                if (k == 'r') {
                    g.restart();
                    status_width_ = reserved_status_width(g.name);
                    resume(now);
                }
                break;
        }
        return true;  // the game view swallows every event
    }

    void resume(Clock::time_point now) {
        paused_ = false;
        fixed_.reset(now);
    }

    Element name_view() {
        const std::string goal = std::to_string(xonix::rules.capture_percent) + "%";
        return vbox({
                   text("XonixM23") | bold | hcenter,
                   separator(),
                   text("Leave the safe border to draw a line across the field."),
                   text("Close it to claim every area that holds no enemy."),
                   text("Claim " + goal + " to clear a level; a hit on you or your line costs a life."),
                   separator(),
                   hbox({text("Name: "), input_->Render() | size(WIDTH, EQUAL, 17)}),
                   text("Enter starts  Esc quits") | dim,
               }) |
               border | center;
    }

    Element game_view() const {
        const xonix::Game& g = *game_;
        const Fit fit = measure_now();
        if (!fit.ok()) {
            // Short lines, so the required size stays readable on a narrow terminal.
            return vbox({
                       text("Terminal too small") | bold | hcenter,
                       text("need " + dims(fit.need_w, fit.need_h)) | hcenter,
                       text("have " + dims(fit.have_w, fit.have_h)) | hcenter,
                       text("Q quits") | dim | hcenter,
                   }) |
                   center;
        }
        // Fixed widths: the board never moves when an overlay appears or the status line grows.
        const int stack_width = std::max(g.grid.size().width, overlay_width);
        Elements layers{Element(std::make_shared<Board>(g)) | hcenter};  // centred under a wider overlay
        if (g.phase == xonix::Phase::LevelClear) {
            layers.push_back(dialog({"Level " + std::to_string(g.level) + " cleared", "Enter to continue"}));
        } else if (g.phase == xonix::Phase::GameOver) {
            layers.push_back(dialog({"Game over — R restart · Q quit", "Final score " + std::to_string(g.score)}));
        } else if (paused_) {
            layers.push_back(dialog({"Paused", "P to resume · Q quit"}));
        }
        return vbox({
                   text(status_line(g)) | hcenter,
                   dbox(std::move(layers)) | size(WIDTH, EQUAL, stack_width) | hcenter,
                   text(help_line) | hcenter,
               }) |
               size(WIDTH, EQUAL, fit.need_w) | center;
    }

    App& app_;
    xonix::Size board_;
    std::string name_;
    Component input_;
    std::optional<xonix::Game> game_;
    bool paused_ = false;
    int status_width_ = 0;  // reserved_status_width() of the current game
    xonix::FixedStep fixed_{xonix::rules.step, xonix::rules.max_catch_up_steps};
};

int run(xonix::Size board) {
    App app = App::Fullscreen();
    app.TrackMouse(false);  // keyboard-only game
    Ui ui(app, board);
    const Component root = ui.root();

    // The ticker only wakes the UI thread; it never touches game state.
    // std::thread rather than std::jthread: Apple's libc++ (Xcode 16) lacks jthread/stop_token by default.
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;  // guarded by mutex
    std::thread ticker([&] {
        std::unique_lock lock(mutex);
        while (!wake.wait_for(lock, frame_interval, [&stop] { return stop; })) {
            app.PostEvent(Event::Custom);
        }
    });
    // Stop and join the ticker before the App goes away, so nothing is posted to a dead App.
    // Also on an exception: destroying a joinable std::thread would terminate the program.
    const auto stop_ticker = [&] {
        {
            const std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_all();
        ticker.join();
    };

    try {
        app.Loop(root);
    } catch (...) {
        stop_ticker();
        throw;
    }
    stop_ticker();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    auto opts = xonix::parse_options(args);
    if (!opts) {
        std::cerr << "error: " << opts.error() << '\n' << xonix::usage_text;
        return 2;
    }
    if (opts->help) {
        std::cout << xonix::usage_text;
        return 0;
    }
    return run(opts->board);
}
