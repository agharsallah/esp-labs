// Lesson 03b: display showcase, on the same display wiring as lesson 03.
//
// Four scenes, each pushing the screen a different way:
//   1. 3D torus    rotation, projection, lighting, depth sorting
//   2. Plasma      every pixel computed from sine waves, every frame
//   3. Fireworks   a particle system with gravity and fading trails
//   4. Dashboard   live chip temperature, memory use and frame timing
//
// Two variations, same scenes (pick one with the PlatformIO environment):
//   03b-showcase          variation 1: BOOT button. click = next, long press = auto-cycle
//   03b-showcase-button   variation 2: push button on GPIO 5 (+ BOOT). click = next,
//                         double-click = previous, long press = auto-cycle
// Serial monitor, both variations: next | prev | scene <1-4> | auto on|off | hud | help
//
// New ideas in this lesson:
//   - Our own frame buffer in fast internal RAM (see frame_buffer.h)
//   - Splitting a program into small parts with one shared contract (scene.h)
//   - Measuring: we time drawing and sending separately, so we know what is slow
//   - Variation 2: interrupts and debouncing for buttons (see button.h)
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SPI.h>
#include <esp_heap_caps.h>

#include "color.h"
#include "controls.h"
#include "frame_buffer.h"
#include "pins.h"
#include "scenes/dashboard.h"
#include "scenes/fireworks.h"
#include "scenes/plasma.h"
#include "scenes/torus.h"
#include "stats.h"

namespace {

constexpr int16_t kW = 320, kH = 170;
constexpr uint32_t kSpiHz = 40'000'000;
constexpr uint32_t kAutoCycleMs = 12'000;  // time per scene when auto-cycling
constexpr uint32_t kBannerMs = 2'000;      // how long the scene title stays up

Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
FrameBuffer fb(kW, kH);
Stats stats;

TorusScene torus;
PlasmaScene plasma;
FireworksScene fireworks;
DashboardScene dashboard(stats);
Scene *const scenes[] = {&torus, &plasma, &fireworks, &dashboard};
constexpr int kSceneCount = sizeof(scenes) / sizeof(scenes[0]);

struct {
  int scene = 0;
  uint32_t sceneStartedAt = 0;
  bool autoCycle = true;
  bool hud = true;
} state;

// ---- Scene switching ------------------------------------------------------------------------

void showScene(int index) {
  state.scene = (index % kSceneCount + kSceneCount) % kSceneCount;
  state.sceneStartedAt = millis();
  scenes[state.scene]->enter(fb);
  log_i("scene %d/%d: %s", state.scene + 1, kSceneCount, scenes[state.scene]->name());
}

// ---- Overlays drawn on top of every scene ---------------------------------------------------

// The scene title slides down from the top, waits, and slides back up.
void drawBanner(uint32_t sinceStart) {
  if (sinceStart >= kBannerMs) return;
  constexpr uint32_t slide = 250;
  float k = 1;
  if (sinceStart < slide) k = sinceStart / float(slide);
  else if (sinceStart > kBannerMs - slide) k = (kBannerMs - sinceStart) / float(slide);
  k = 1 - (1 - k) * (1 - k);  // ease out

  char text[40];
  snprintf(text, sizeof(text), "%d/%d  %s", state.scene + 1, kSceneCount, scenes[state.scene]->name());
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int bw = w + 28, bh = 28, bx = (kW - bw) / 2, by = int(-bh + (bh + 8) * k);
  fb.fillRoundRect(bx, by, bw, bh, 14, pal::ink);
  fb.setTextColor(pal::night);
  fb.setCursor(bx + 14 - x1, by + 19);
  fb.print(text);
}

// Frame timing in the bottom-left corner: the numbers we will improve in the speed lesson.
void drawHud() {
  if (!state.hud) return;
  char text[48];
  snprintf(text, sizeof(text), "%2.0f fps  draw %4.1f  send %4.1f ms%s", stats.fps, stats.drawMs, stats.pushMs,
           state.autoCycle ? "  auto" : "");
  fb.setFont(nullptr);
  fb.setTextSize(1);
  fb.fillRect(0, kH - 12, int(strlen(text)) * 6 + 8, 12, pal::night);
  fb.setTextColor(pal::amber);
  fb.setCursor(4, kH - 10);
  fb.print(text);
}

// Variation 2 only: a dot in the bottom-right corner that lights while a button is held down.
void drawButtonDot() {
  if (!controls::showsPressDot()) return;
  fb.fillCircle(kW - 10, kH - 10, 5, controls::anyDown() ? pal::teal : pal::dim);
  fb.drawCircle(kW - 10, kH - 10, 6, pal::night);
}

// ---- Input -----------------------------------------------------------------------------------

void printHelp() {
  Serial.println();
  Serial.println("Commands: next | prev | scene <1-4> | auto on|off | hud | help");
  Serial.println(controls::help());
}

void handleCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.isEmpty()) return;
  if (line == "next") return showScene(state.scene + 1);
  if (line == "prev") return showScene(state.scene - 1);
  if (line == "hud") {
    state.hud = !state.hud;
    log_i("hud %s", state.hud ? "on" : "off");
    return;
  }
  if (line == "auto on" || line == "auto off") {
    state.autoCycle = line == "auto on";
    log_i("auto-cycle %s", state.autoCycle ? "on" : "off");
    return;
  }
  if (line.startsWith("scene ")) {
    const long n = line.substring(6).toInt();
    if (n < 1 || n > kSceneCount) {
      log_w("scene needs a number 1-%d, got '%s'", kSceneCount, line.substring(6).c_str());
      return;
    }
    return showScene(n - 1);
  }
  if (line == "help" || line == "?") return printHelp();
  log_w("unknown command '%s' (type help)", line.c_str());
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(buffer);
      buffer = "";
    } else if (buffer.length() < 32) {
      buffer += c;
    }
  }
}

void pollControls() {
  switch (controls::poll()) {
    case Action::NextScene:
      showScene(state.scene + 1);
      break;
    case Action::PreviousScene:
      showScene(state.scene - 1);
      break;
    case Action::ToggleAutoCycle:
      state.autoCycle = !state.autoCycle;
      log_i("auto-cycle %s", state.autoCycle ? "on" : "off");
      break;
    case Action::None:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(170, 320);
  tft.setSPISpeed(kSpiHz);
  tft.setRotation(1);
  tft.fillScreen(pal::night);

  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (!fb.begin()) {
    log_e("No memory for the %u-byte frame buffer, internal or PSRAM. Stopping.", unsigned(fb.bytesUsed()));
    return;
  }
  log_i("Frame buffer: %u KB in %s (largest free internal block was %u KB)", unsigned(fb.bytesUsed() / 1024),
        fb.internal() ? "internal RAM (fast)" : "PSRAM (slower)", unsigned(largest / 1024));

  controls::begin();
  randomSeed(esp_random());
  printHelp();
  showScene(0);
}

void loop() {
  if (fb.pixels() == nullptr) {
    delay(1000);
    return;
  }
  pollSerial();
  pollControls();

  static uint32_t lastFrame = millis(), lastLog = 0, lastTemp = 0;
  const uint32_t now = millis();
  const float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;

  if (state.autoCycle && now - state.sceneStartedAt >= kAutoCycleMs) showScene(state.scene + 1);
  if (now - lastTemp >= 1000) {  // the temperature changes slowly; once a second is plenty
    lastTemp = now;
    stats.tempC = temperatureRead();
  }

  // Draw the whole picture in memory, then send it in one go. Time both halves.
  const uint32_t t0 = micros();
  scenes[state.scene]->render(fb, (now - state.sceneStartedAt) / 1000.0f, dt);
  drawBanner(now - state.sceneStartedAt);
  drawHud();
  drawButtonDot();
  const uint32_t t1 = micros();
  tft.drawRGBBitmap(0, 0, fb.pixels(), kW, kH);
  const uint32_t t2 = micros();
  stats.add((t1 - t0) / 1000.0f, (t2 - t1) / 1000.0f, dt);

  if (now - lastLog >= 5000) {
    lastLog = now;
    log_i("%-14s %5.1f fps | draw %5.1f ms | send %5.1f ms | %.1f C", scenes[state.scene]->name(), stats.fps,
          stats.drawMs, stats.pushMs, stats.tempC);
  }
}
