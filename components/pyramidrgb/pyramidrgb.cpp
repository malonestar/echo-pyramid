#include "pyramidrgb.h"

namespace esphome {
namespace pyramidrgb {

static const char *const TAG = "pyramidrgb";

void PyramidRGBComponent::setup() {
  ESP_LOGI(TAG, "PyramidRGB init (STM32 RGB controller at 0x%02X)", this->address_);

  if (!this->set_strip_brightness(initial_strip_, initial_brightness_)) {
    ESP_LOGW(TAG, "Failed to set initial brightness for strip %u", initial_strip_);
  }
  // Apply optional initial white level directly (setup runs before loop, so
  // write to hardware immediately rather than deferring to loop()).
  if (initial_white_level_ > 0) {
    uint8_t v = initial_white_level_;
    if (initial_strip_ == 1) {
      channel_colors_[0][0] = v; channel_colors_[0][1] = v; channel_colors_[0][2] = v;
      channel_colors_[1][0] = v; channel_colors_[1][1] = v; channel_colors_[1][2] = v;
      write_channel_now_(0);
      write_channel_now_(1);
    } else if (initial_strip_ == 2) {
      channel_colors_[2][0] = v; channel_colors_[2][1] = v; channel_colors_[2][2] = v;
      channel_colors_[3][0] = v; channel_colors_[3][1] = v; channel_colors_[3][2] = v;
      write_channel_now_(2);
      write_channel_now_(3);
    }
  }
}

void PyramidRGBComponent::loop() {
  // Flush any channels whose color buffer was updated since the last tick.
  // Because ESPHome's light component calls write_state() separately for R, G,
  // and B, all three updates accumulate in channel_colors_[] before this runs,
  // so the hardware never sees a partial/intermediate color state.
  for (uint8_t ch = 0; ch < NUM_RGB_CHANNELS; ch++) {
    if (channel_dirty_[ch]) {
      write_channel_now_(ch);
      channel_dirty_[ch] = false;
    }
  }
}

void PyramidRGBComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "PyramidRGB Component");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "strip=%u brightness=%u initial_white=%u", initial_strip_, initial_brightness_, initial_white_level_);
  ESP_LOGCONFIG(TAG, "log_dimming=%s gamma=%.2f high_pwm_freq=%s power_save=%s internal_clk=%s",
                logarithmic_dimming_ ? "true" : "false",
                gamma_,
                high_pwm_freq_ ? "true" : "false",
                power_save_mode_ ? "true" : "false",
                use_internal_clk_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "ref_current=%.2f R=%.2f G=%.2f B=%.2f scales R=%.2f G=%.2f B=%.2f",
                ref_current_, red_current_, green_current_, blue_current_, red_scale_, green_scale_, blue_scale_);
}

bool PyramidRGBComponent::set_strip_brightness(uint8_t strip, uint8_t brightness) {
  if (strip < 1 || strip > 2) return false;
  if (brightness > 100) brightness = 100;
  uint8_t reg = (strip == 1) ? RGB1_BRIGHTNESS_REG_ADDR : RGB2_BRIGHTNESS_REG_ADDR;
  uint8_t b = (uint8_t)((brightness * 255) / 100);
  bool ok = this->write_byte(reg, b);
  ESP_LOGD(TAG, "Set brightness: strip=%u reg=0x%02X b=%u (%%=%u) -> %s",
           strip, reg, b, brightness, ok ? "OK" : "FAIL");
  return ok;
}

uint8_t PyramidRGBComponent::channel_base_addr_(uint8_t channel) const {
  switch (channel) {
    case 0: return RGB_CH1_I1_COLOR_REG_ADDR;
    case 1: return RGB_CH2_I1_COLOR_REG_ADDR;
    case 2: return RGB_CH4_I1_COLOR_REG_ADDR;
    case 3: return RGB_CH3_I1_COLOR_REG_ADDR;
    default: return RGB_CH1_I1_COLOR_REG_ADDR;
  }
}

bool PyramidRGBComponent::write_color_block_(uint8_t base_reg_addr, const uint8_t *color_bytes, size_t len) {
  const size_t total = len + 1;
  uint8_t *buf = (uint8_t *) malloc(total);
  if (buf == nullptr) return false;
  buf[0] = base_reg_addr;
  memcpy(buf + 1, color_bytes, len);
  bool ok = this->write(buf, total);
  free(buf);
  return ok;
}

bool PyramidRGBComponent::write_channel_now_(uint8_t channel) {
  if (channel >= NUM_RGB_CHANNELS) return false;
  uint8_t r = channel_colors_[channel][0];
  uint8_t g = channel_colors_[channel][1];
  uint8_t b = channel_colors_[channel][2];
  uint8_t base = channel_base_addr_(channel);

  // Use the same proven per-LED write pattern as the original component.
  // A single 29-byte burst write was tested but caused flickers on some channels,
  // likely because the STM32 controller's auto-increment has a shorter limit.
  bool all_ok = true;
  for (uint8_t i = 0; i < NUM_LEDS_PER_GROUP; i++) {
    uint8_t hardware_index = i;
    if (channel == 0 || channel == 1) {
      hardware_index = NUM_LEDS_PER_GROUP - 1 - i;
    }
    uint8_t reg = base + (hardware_index * 4);
    uint8_t led_bytes[4] = {b, g, r, 0x00};
    all_ok = all_ok && write_color_block_(reg, led_bytes, sizeof(led_bytes));
  }
  ESP_LOGD(TAG, "Write channel %u RGB=(%u,%u,%u) -> %s", channel, r, g, b, all_ok ? "OK" : "FAIL");
  return all_ok;
}

bool PyramidRGBComponent::set_channel_color(uint8_t channel, uint8_t r, uint8_t g, uint8_t b) {
  if (channel >= NUM_RGB_CHANNELS) return false;
  channel_colors_[channel][0] = r;
  channel_colors_[channel][1] = g;
  channel_colors_[channel][2] = b;
  channel_dirty_[channel] = true;
  return true;
}

bool PyramidRGBComponent::set_channel_color_component(uint8_t channel, RGBColorChannel color, uint8_t value) {
  if (channel >= NUM_RGB_CHANNELS) return false;
  switch (color) {
    case COLOR_R: channel_colors_[channel][0] = value; break;
    case COLOR_G: channel_colors_[channel][1] = value; break;
    case COLOR_B: channel_colors_[channel][2] = value; break;
    default: return false;
  }
  channel_dirty_[channel] = true;
  return true;
}

uint8_t PyramidRGBComponent::map_level(RGBColorChannel color, float level) const {
  if (level <= 0.0f) return 0;
  if (level >= 1.0f) level = 1.0f;
  float x = level;
  // Apply gamma/logarithmic dimming if enabled
  float g = gamma_;
  if (logarithmic_dimming_ && g > 0.0f && g != 1.0f) {
    // Simple gamma curve
    x = powf(x, g);
  }
  // Apply per-color scale based on configured nominal currents
  float scale = 1.0f;
  switch (color) {
    case COLOR_R: scale = red_scale_; break;
    case COLOR_G: scale = green_scale_; break;
    case COLOR_B: scale = blue_scale_; break;
    default: break;
  }
  x *= scale;
  if (x > 1.0f) x = 1.0f;
  if (x < 0.0f) x = 0.0f;
  return (uint8_t) (x * 255.0f + 0.5f);
}

}  // namespace pyramidrgb
}  // namespace esphome
