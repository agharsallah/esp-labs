// Every scene follows the same small contract, so main.cpp can switch between them
// without knowing how any of them draws.
#pragma once

#include "frame_buffer.h"

class Scene {
 public:
  virtual ~Scene() = default;
  virtual const char *name() const = 0;
  // Called each time the scene comes on screen.
  virtual void enter(FrameBuffer &) {}
  // t = seconds since the scene came on screen, dt = seconds since the last frame.
  virtual void render(FrameBuffer &fb, float t, float dt) = 0;
};
