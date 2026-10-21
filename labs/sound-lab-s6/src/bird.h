// Clap Bird: the game's rules, without any drawing. Pure C++ (no Arduino), host-tested in
// labs/sound-lab-s6/test/host/sound_games_test.cpp.
//
// Physics in one line each, run 60 times a second whatever the screen does:
//   gravity pulls:  vy += kGravity x dt     a clap kicks:   vy = kFlap (upwards = negative y)
//   the bird moves: y  += vy x dt           pipes slide:    x  -= kSpeed x dt
// The bird stays at x = kBirdX; the world moves past it. Touch a pipe, the ceiling or the
// ground and the round is over. Every pipe you get past is a point.
#pragma once

#include <stdint.h>

namespace bird {

// The playing field: the screen between the header (30) and the footer (158).
constexpr float kTop = 30, kBottom = 158, kWidth = 320;
constexpr float kBirdX = 70, kRadius = 6;
constexpr float kGravity = 420, kFlap = -150, kSpeed = 72;  // px/s², px/s, px/s
constexpr float kPipeW = 26, kGap = 58, kSpacing = 130;
constexpr int kPipes = 3;

struct Pipe {
  float x;
  float gapY;  // the centre of the opening
  bool passed;
};

class Game {
 public:
  enum class State : uint8_t { Ready, Playing, Over };

  explicit Game(uint32_t seed = 1) : seed_(seed ? seed : 1) { reset(); }

  void reset() {
    state_ = State::Ready;
    y_ = (kTop + kBottom) / 2;
    vy_ = 0;
    score_ = 0;
    for (int i = 0; i < kPipes; i++) pipes_[i] = {kWidth + 40 + i * kSpacing, randomGap(), false};
  }

  // A clap: starts a round from Ready, flaps while playing. Over: ignored (the view restarts).
  void flap() {
    if (state_ == State::Over) return;
    state_ = State::Playing;
    vy_ = kFlap;
  }

  // Advance by dt seconds (call with small steps: 1/60 s).
  void step(float dt) {
    if (state_ != State::Playing) return;
    vy_ += kGravity * dt;
    y_ += vy_ * dt;
    for (Pipe &p : pipes_) {
      p.x -= kSpeed * dt;
      if (!p.passed && p.x + kPipeW < kBirdX - kRadius) p.passed = true, score_++;
      if (p.x + kPipeW < 0) {  // off the left edge: reuse it behind the last one
        float far = 0;
        for (const Pipe &q : pipes_) far = q.x > far ? q.x : far;
        p = {far + kSpacing, randomGap(), false};
      }
      if (hits(p)) state_ = State::Over;
    }
    if (y_ - kRadius <= kTop || y_ + kRadius >= kBottom) state_ = State::Over;
  }

  State state() const { return state_; }
  float y() const { return y_; }
  float vy() const { return vy_; }
  int score() const { return score_; }
  const Pipe &pipe(int i) const { return pipes_[i]; }
  // For tests: put a pipe exactly where you want it.
  void setPipe(int i, const Pipe &p) { pipes_[i] = p; }

 private:
  // The bird as a square of side 2 x radius against the pipe's two rectangles.
  bool hits(const Pipe &p) const {
    if (kBirdX + kRadius < p.x || kBirdX - kRadius > p.x + kPipeW) return false;
    return y_ - kRadius < p.gapY - kGap / 2 || y_ + kRadius > p.gapY + kGap / 2;
  }

  // A small random number generator (xorshift): the same seed = the same course.
  float randomGap() {
    seed_ ^= seed_ << 13, seed_ ^= seed_ >> 17, seed_ ^= seed_ << 5;
    const float lo = kTop + kGap / 2 + 8, hi = kBottom - kGap / 2 - 8;
    return lo + (seed_ % 1000) / 1000.0f * (hi - lo);
  }

  State state_ = State::Ready;
  float y_ = 0, vy_ = 0;
  int score_ = 0;
  Pipe pipes_[kPipes];
  uint32_t seed_;
};

}  // namespace bird
