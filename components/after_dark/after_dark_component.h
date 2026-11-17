#pragma once
/*
 * The ESPHome end of the After Dark savers: draws the chosen saver straight
 * into a display's buffer and pushes it, at its own frame rate, instead of
 * going through a display lambda (which would clear and redraw everything).
 *
 * It drives either a pixel display (colour, or 1-bit like the SSD1306/SH1106
 * OLEDs, which get baked black-and-white art) or a character LCD (lcd_pcf8574,
 * lcd_gpio), where the savers draw with the eight user-defined glyphs.
 *
 * The savers themselves are in the ad_*.cpp files and know nothing of ESPHome.
 */
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display.h"
#ifdef AD_USE_LCD
#include "esphome/components/lcd_base/lcd_display.h"
#endif
#ifdef AD_USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "ad_core.h"
#include "ad_live.h"
#include "ad_meters.h"
#include "ad_text.h"

#include <memory>
#include <vector>
#include <string>
#include <vector>

namespace esphome {
namespace after_dark {

/* The display's own drawing, which screensaver mode moves aside while a
   saver runs and puts back after. They are protected; a pointer to member
   formed in a derived class reaches them without changing the display. */
struct DisplayInternals : display::Display {
  using Writer = decltype(writer_);
  static auto writer() { return &DisplayInternals::writer_; }
  static auto page() { return &DisplayInternals::page_; }
  static auto auto_clear() { return &DisplayInternals::auto_clear_enabled_; }
};

#ifdef AD_USE_LCD
/* The character LCD's own byte writers, protected too, reached the same way:
   frame-rate modulation rewrites a few cells a pass, not the whole panel. */
struct LcdInternals : lcd_base::LCDDisplay {
  using Send = void (lcd_base::LCDDisplay::*)(uint8_t, bool);
  using Command = void (lcd_base::LCDDisplay::*)(uint8_t);
  static Send send_fn() { return &LcdInternals::send; }
  static Command command_fn() { return &LcdInternals::command_; }
};
#endif

class DisplaySurface : public ::adcore::Surface {
 public:
  void attach(display::Display *d) {
    display_ = d;
    width_ = d->get_width();
    height_ = d->get_height();
    mono_ = d->get_display_type() == display::DISPLAY_TYPE_BINARY;
    set_subframes(subframes_);
  }
  int width() const override { return width_; }
  int height() const override { return height_; }
  bool mono() const override { return mono_; }
  void pixel(int x, int y, ::adcore::Rgb c) override;
  void fill_rect(int x, int y, int w, int h, ::adcore::Rgb c) override;

  /* Frame-rate modulation on a 1-bit panel: 2-4 subframes, 1 off. Each
     pixel's shade is kept (a byte each), so that every subframe the
     in-between shades can be drawn again, a grey dot lit in only some of
     them: lit for a share of the time as well as a grey area having a share
     of its dots lit. What is drawn already isn't known
     here, so start the saver again after a change. */
  void set_subframes(int n);
  bool modulating() const { return !lum_.empty(); }
  void next_subframe();

 protected:
  display::Display *display_{nullptr};
  int width_{0}, height_{0};
  bool mono_{false};
  int subframes_{1}, phase_{0};
  std::vector<uint8_t> lum_;
};

class AfterDarkComponent : public Component {
 public:
  void set_display(display::Display *display) { display_ = display; }
#ifdef AD_USE_LCD
  void set_lcd(lcd_base::LCDDisplay *lcd, int cols, int rows) {
    lcd_ = lcd;
    lcd_cols_ = cols;
    lcd_rows_ = rows;
  }
#endif
#ifdef AD_USE_TIME
  void set_time(time::RealTimeClock *time) { time_ = time; }
#endif
  void set_fps(int fps) { fps_ = fps; }
  /* Screensaver mode: the display stays the YAML's own until start(). */
  void set_screensaver_mode(bool on) { screensaver_mode_ = on; }
  /* Frame-rate modulation on a 1-bit panel: 2-4 subframes (1 is off). The
     screen is then rewritten as fast as it goes between the saver's frames.
     Only the savers that draw in-between shades change. */
  /* The dolphins' sky on a character LCD: where the panel is (degrees) and
     the weather (Home Assistant's condition names). */
  void set_sky_latitude(float degrees);
  void set_sky_longitude(float degrees);
  void set_sky_weather(const std::string &condition);
  void set_frame_rate_modulation(int subframes);
  int get_frame_rate_modulation() const { return subframes_; }
  /* How many subframes it uses when on (what the YAML asked for, or 2). */
  int frame_rate_modulation_when_on() const { return subframes_on_; }
  /* Character LCD: while this returns lines, they are shown instead. */
  void set_overlay(std::function<std::vector<std::string>()> &&f) { overlay_ = std::move(f); }

  /* Screensaver mode. start() runs the selected saver (a random one for
     "random"), stop() gives the display back and redraws it. */
  void start();
  void stop();
  bool is_running() const { return running_; }

  /* DOS Shell's live commands: a command, then the entities it prints. Empty
     strings for attribute, label and unit mean the state, the friendly name
     and the unit of measurement, as Home Assistant sends them. */
  void add_shell_command(const std::string &name);
  void add_shell_entity(const std::string &entity_id, const std::string &attribute, const std::string &label,
                        const std::string &unit);
  void set_shell_classic(bool classic) { ::adcore::shell_classic = classic; }
  /* A line worked out when it is printed, a command's lines from a lambda,
     and when a command is due; each applies to the last command added. */
  void add_shell_text(std::function<std::string()> &&f);
  void set_shell_lines(std::function<std::vector<std::string>()> &&f);
  void set_shell_when(std::function<bool()> &&f);
  void set_shell_prompt(const std::string &p) { ::adcore::set_live(::adcore::shell_style.prompt, p); }
  void add_shell_banner(const std::string &line) { ::adcore::shell_style.banner.push_back(line); }
  void set_shell_bad_command(const std::string &f) { ::adcore::shell_style.bad_command = f; }
  void set_shell_lowercase(bool on) { ::adcore::shell_style.lowercase = on; }
  void set_initial_saver(const std::string &name) { initial_ = name; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  /* After the display has its buffer. */
  float get_setup_priority() const override { return setup_priority::LATE; }

  /* Switch saver; "off" blanks the screen and stops drawing. In
     screensaver mode it chooses what start() runs ("random", or "off" for
     nothing) and switches a running one over. */
  bool set_saver(const std::string &name);
  const std::string &get_saver() const { return current_; }
  void add_on_saver_callback(std::function<void(const std::string &)> &&cb) { callback_.add(std::move(cb)); }

 protected:
  bool text_mode_() const { return lcd_cols_ > 0; }
  bool run_(const std::string &name);
  std::string pick_random_();
  void blank_();
  void push_();
#ifdef AD_USE_LCD
  void push_text_modulated_();
  /* What the panel shows, cell by cell, when frame-rate modulation writes
     it; and which pass this is (dim cells blank on the odd ones). */
  uint8_t panel_[::adcore::TextScreen::MAX_ROWS][::adcore::TextScreen::MAX_COLS];
  bool panel_ok_{false};
  uint8_t text_phase_{0};
#endif
  void count_subframe_(uint32_t now);

  display::Display *display_{nullptr};
  DisplaySurface surface_;
  std::unique_ptr<::adcore::Saver> saver_;
#ifdef AD_USE_LCD
  lcd_base::LCDDisplay *lcd_{nullptr};
#endif
  int lcd_cols_{0}, lcd_rows_{0};
#ifdef AD_USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
  ::adcore::TextScreen text_;
  std::unique_ptr<::adcore::TextSaver> text_saver_;

  std::string initial_;
  std::string current_;
  int fps_{30};
  int subframes_{1}, subframes_on_{2};
  uint32_t sub_ms_{0}, sent_{0}, rate_ms_{0};
  std::string running_name_;
  struct ShellEntity {
    size_t command, line;
    std::string entity_id, attribute;
    bool own_label, own_unit;
  };
  std::vector<ShellEntity> shell_entities_;
  void subscribe_shell_();
  uint32_t last_ms_{0};
  bool screensaver_mode_{false}, running_{false};
  DisplayInternals::Writer saved_writer_{};
  display::DisplayPage *saved_page_{nullptr};
  bool saved_auto_clear_{true};
  std::string last_random_;
  std::function<std::vector<std::string>()> overlay_;
  bool overlay_shown_{false};
  std::vector<std::string> last_overlay_;
  HighFrequencyLoopRequester high_freq_;
  CallbackManager<void(const std::string &)> callback_;
};

}  // namespace after_dark
}  // namespace esphome
