// Scene 1: a shaded 3D torus (a ring), built from flat four-sided faces.
//
// Every frame:
//   1. rotate each corner point in 3D
//   2. project it onto the 2D screen (things further away get closer to the centre)
//   3. throw away faces that point away from us (backface culling)
//   4. light each face by how directly it faces the light
//   5. sort faces far-to-near and paint them in that order (painter's algorithm)
#pragma once

#include <math.h>

#include <algorithm>

#include "color.h"
#include "../scene.h"

class TorusScene : public Scene {
 public:
  TorusScene() {
    // Corner points on the ring surface, and the outward direction at each face centre.
    for (int i = 0; i < kU; i++) {
      for (int j = 0; j < kV; j++) {
        const float u = i * kTau / kU, v = j * kTau / kV;
        points_[i * kV + j] = {(kR + kr * cosf(v)) * cosf(u), (kR + kr * cosf(v)) * sinf(u), kr * sinf(v)};
        const float uc = (i + 0.5f) * kTau / kU, vc = (j + 0.5f) * kTau / kV;
        normals_[i * kV + j] = {cosf(vc) * cosf(uc), cosf(vc) * sinf(uc), sinf(vc)};
      }
    }
    // A night-sky gradient behind the ring, one colour per row.
    for (int y = 0; y < kH; y++) sky_[y] = mix(rgb(16, 22, 40), rgb(4, 6, 12), y * 255 / kH);
  }

  const char *name() const override { return "3D torus"; }

  void render(FrameBuffer &fb, float t, float) override {
    for (int y = 0; y < kH; y++) fb.drawFastHLine(0, y, kW, sky_[y]);

    // Rotation angles. Two different speeds make the motion feel organic.
    const float ca = cosf(t * 0.9f), sa = sinf(t * 0.9f);
    const float cb = cosf(t * 0.55f), sb = sinf(t * 0.55f);
    auto rotate = [&](const Vec &p) {
      const float y1 = p.y * ca - p.z * sa, z1 = p.y * sa + p.z * ca;
      return Vec{p.x * cb + z1 * sb, y1, -p.x * sb + z1 * cb};
    };

    // 1 + 2: rotate and project every corner point.
    for (int k = 0; k < kU * kV; k++) {
      const Vec r = rotate(points_[k]);
      const float z = r.z + kCamera;
      sx_[k] = int16_t(kW / 2 + r.x * kFocal / z);
      sy_[k] = int16_t(kH / 2 + r.y * kFocal / z);
      sz_[k] = z;
    }

    // 3 + 4: keep faces that point at us, and light them.
    visible_ = 0;
    for (int i = 0; i < kU; i++) {
      for (int j = 0; j < kV; j++) {
        const int a = i * kV + j, b = ((i + 1) % kU) * kV + j;
        const int c = ((i + 1) % kU) * kV + (j + 1) % kV, d = i * kV + (j + 1) % kV;
        const Vec n = rotate(normals_[a]);
        const Vec centre = rotate(points_[a]);
        // The face points at us when its normal and the line from the camera to it disagree.
        if (n.x * centre.x + n.y * centre.y + n.z * (centre.z + kCamera) >= 0) continue;

        const float diffuse = std::max(0.0f, n.x * kLight.x + n.y * kLight.y + n.z * kLight.z);
        const uint8_t level = uint8_t(40 + 215 * diffuse);
        const bool stripe = ((i / 2) + j) % 2 == 0;
        Face &f = faces_[visible_++];
        f.depth = sz_[a] + sz_[b] + sz_[c] + sz_[d];
        f.color = mix(pal::night, stripe ? pal::amber : pal::teal, level);
        f.corners[0] = a, f.corners[1] = b, f.corners[2] = c, f.corners[3] = d;
      }
    }

    // 5: paint far faces first so near ones cover them.
    std::sort(faces_, faces_ + visible_, [](const Face &l, const Face &r) { return l.depth > r.depth; });
    for (int k = 0; k < visible_; k++) {
      const int16_t *q = faces_[k].corners;
      fb.fillTriangle(sx_[q[0]], sy_[q[0]], sx_[q[1]], sy_[q[1]], sx_[q[2]], sy_[q[2]], faces_[k].color);
      fb.fillTriangle(sx_[q[0]], sy_[q[0]], sx_[q[2]], sy_[q[2]], sx_[q[3]], sy_[q[3]], faces_[k].color);
    }

    fb.setFont(nullptr);
    fb.setTextSize(1);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 8);
    fb.printf("%d faces, %d drawn", kU * kV, visible_);
  }

 private:
  struct Vec {
    float x, y, z;
  };
  struct Face {
    float depth;
    uint16_t color;
    int16_t corners[4];
  };

  static constexpr int kW = 320, kH = 170;
  static constexpr int kU = 24, kV = 10;              // segments around the ring and around the tube
  static constexpr float kR = 1.0f, kr = 0.42f;        // ring radius, tube radius
  static constexpr float kCamera = 3.4f, kFocal = 175;  // camera distance and lens "zoom"
  static constexpr float kTau = 6.2831853f;
  static constexpr Vec kLight = {-0.45f, -0.6f, -0.66f};  // towards the light: up, left, in front

  Vec points_[kU * kV];
  Vec normals_[kU * kV];
  int16_t sx_[kU * kV], sy_[kU * kV];
  float sz_[kU * kV];
  Face faces_[kU * kV];
  int visible_ = 0;
  uint16_t sky_[kH];
};
