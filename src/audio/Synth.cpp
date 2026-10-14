// Built-in placeholder sounds, synthesized in code so the engine ships no
// third-party audio: every sample here is generated from the formulas below
// (CC0 by construction). Deterministic (fixed-seed noise), mono.

#include "audio/Audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace ts {
namespace audio {

namespace {

constexpr float kTau = 6.28318530718f;

struct Noise {
  std::uint32_t s = 0x1234567u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFFu) / 32767.5f - 1.0f;
  }
};

std::shared_ptr<Clip> make(int rate, float seconds,
                           const std::function<float(float t, Noise&)>& fn,
                           float peak = 0.8f) {
  auto clip = std::make_shared<Clip>();
  clip->channels = 1;
  clip->rate = rate;
  const std::size_t n = static_cast<std::size_t>(seconds * static_cast<float>(rate));
  clip->samples.resize(n);
  Noise noise;
  float mx = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(rate);
    clip->samples[i] = fn(t, noise);
    mx = std::max(mx, std::fabs(clip->samples[i]));
  }
  // Normalise, then a 2 ms fade-out so nothing ends on a click.
  const float k = mx > 0.0f ? peak / mx : 0.0f;
  const std::size_t fade = std::min<std::size_t>(n, static_cast<std::size_t>(rate / 500));
  for (std::size_t i = 0; i < n; ++i) {
    float g = k;
    if (i + fade >= n && fade > 0) g *= static_cast<float>(n - i) / static_cast<float>(fade);
    clip->samples[i] *= g;
  }
  return clip;
}

// One-pole low-pass state for noise colouring.
struct LowPass {
  float y = 0.0f;
  float a;
  explicit LowPass(float alpha) : a(alpha) {}
  float operator()(float x) { return y += a * (x - y); }
};

float pluck(float t, float freq) {
  if (t < 0.0f) return 0.0f;
  const float env = std::exp(-t * 6.0f) * std::min(1.0f, t * 400.0f);
  return env * (std::sin(kTau * freq * t) + 0.35f * std::sin(kTau * 2.0f * freq * t) +
                0.15f * std::sin(kTau * 3.0f * freq * t));
}

}  // namespace

const std::vector<std::string>& builtin_names() {
  static const std::vector<std::string> names = {
      "builtin:click", "builtin:step",  "builtin:gunshot", "builtin:mine",
      "builtin:boom",  "builtin:grab",  "builtin:throw",   "builtin:chime",
      "builtin:tune"};
  return names;
}

bool is_builtin(const std::string& ref) { return ref.rfind("builtin:", 0) == 0; }

ClipPtr synth_builtin(const std::string& name, int rate) {
  if (rate < 8000) rate = 8000;
  const std::string n = is_builtin(name) ? name.substr(8) : name;
  if (n == "click") {
    return make(rate, 0.03f, [](float t, Noise&) {
      return std::sin(kTau * 2000.0f * t) * std::exp(-t * 180.0f);
    }, 0.5f);
  }
  if (n == "step") {
    auto lp = std::make_shared<LowPass>(0.08f);
    return make(rate, 0.09f, [lp](float t, Noise& no) {
      return (*lp)(no.next()) * std::exp(-t * 45.0f) +
             0.3f * std::sin(kTau * 90.0f * t) * std::exp(-t * 60.0f);
    }, 0.6f);
  }
  if (n == "gunshot") {
    auto lp = std::make_shared<LowPass>(0.35f);
    return make(rate, 0.35f, [lp](float t, Noise& no) {
      return (*lp)(no.next()) * std::exp(-t * 18.0f) +
             0.8f * std::sin(kTau * 70.0f * t) * std::exp(-t * 14.0f);
    }, 0.9f);
  }
  if (n == "mine") {
    return make(rate, 0.22f, [](float t, Noise& no) {
      const float ring = std::sin(kTau * 1210.0f * t) + 0.7f * std::sin(kTau * 1873.0f * t) +
                         0.5f * std::sin(kTau * 2651.0f * t);
      return ring * std::exp(-t * 22.0f) + no.next() * std::exp(-t * 120.0f);
    }, 0.7f);
  }
  if (n == "boom") {
    auto lp = std::make_shared<LowPass>(0.03f);
    return make(rate, 1.2f, [lp](float t, Noise& no) {
      return 3.0f * (*lp)(no.next()) * std::exp(-t * 3.5f) +
             std::sin(kTau * (55.0f - 20.0f * t) * t) * std::exp(-t * 4.0f);
    }, 0.95f);
  }
  if (n == "grab") {
    return make(rate, 0.08f, [](float t, Noise&) {
      const float f = 300.0f + 4000.0f * t;
      return std::sin(kTau * f * t) * std::exp(-t * 30.0f);
    }, 0.5f);
  }
  if (n == "throw") {
    auto lp = std::make_shared<LowPass>(0.05f);
    return make(rate, 0.28f, [lp](float t, Noise& no) {
      lp->a = 0.03f + 0.4f * t;
      const float env = std::sin(3.14159f * std::min(1.0f, t / 0.28f));
      return (*lp)(no.next()) * env;
    }, 0.6f);
  }
  if (n == "chime") {
    return make(rate, 0.6f, [](float t, Noise&) {
      return (std::sin(kTau * 880.0f * t) + 0.6f * std::sin(kTau * 1320.0f * t)) *
             std::exp(-t * 6.0f);
    }, 0.5f);
  }
  if (n == "tune") {
    // "Saloon Band" placeholder: 8 bars of a plucked I-IV-V-I arpeggio over
    // a bass note, 120 bpm, loops seamlessly (8 s).
    static const float roots[4] = {130.81f, 174.61f, 196.0f, 130.81f};  // C F G C
    static const float steps[8] = {1.0f, 1.25f, 1.5f, 2.0f, 1.5f, 1.25f, 1.0f, 1.5f};
    return make(rate, 8.0f, [](float t, Noise&) {
      const int bar = static_cast<int>(t / 2.0f) % 4;
      const float in_bar = std::fmod(t, 2.0f);
      const int note = static_cast<int>(in_bar / 0.25f);
      const float nt = in_bar - static_cast<float>(note) * 0.25f;
      const float r = roots[bar];
      float s = 0.5f * pluck(nt, r * steps[note]) * (note % 2 ? 0.8f : 1.0f);
      s += 0.6f * pluck(in_bar - std::floor(in_bar), r * 0.5f);  // bass on beats
      return s;
    }, 0.6f);
  }
  return nullptr;
}

}  // namespace audio
}  // namespace ts
