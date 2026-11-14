// Mock data for the Nextion page replacements: what a US (Fahrenheit) panel in this house would
// show. One PageCase per page and variant, in montage order, each with a typical update so the
// cost of a change is measured as well as the cost of the first frame.
#include "Pages.hpp"

#include "BootScreen.hpp"
#include "CoverView.hpp"
#include "EntitiesView.hpp"
#include "HomeView.hpp"
#include "LightView.hpp"
#include "MediaPlayerView.hpp"
#include "MenuView.hpp"
#include "ScreensaverView.hpp"
#include "ThermostatView.hpp"
#include "Ui.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

// --- Album art -----------------------------------------------------------------------------------
// Generated, as there is no image to ship: a "photo" (sunset over hills, with the per-pixel grain a
// JPEG-decoded photo has, so almost every pixel differs from its neighbour) and a flat graphic
// cover (smooth gradient and solid shapes, as many album covers are). Both 180 x 180 RGB565, the
// size the manager scales album art to for the panel.

static uint16_t rgb565(double r, double g, double b) {
  auto c = [](double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; };
  return (uint16_t)(((c(r) >> 3) << 11) | ((c(g) >> 2) << 5) | (c(b) >> 3));
}

static lv_image_dsc_t make_image(std::vector<uint16_t> &pixels, int size) {
  lv_image_dsc_t image = {};
  image.header.magic = LV_IMAGE_HEADER_MAGIC;
  image.header.cf = LV_COLOR_FORMAT_RGB565;
  image.header.w = size;
  image.header.h = size;
  image.header.stride = size * 2;
  image.data_size = pixels.size() * 2;
  image.data = reinterpret_cast<const uint8_t *>(pixels.data());
  return image;
}

static const lv_image_dsc_t *photo_art() {
  static std::vector<uint16_t> pixels;
  static lv_image_dsc_t image;
  if (pixels.empty()) {
    const int n = MediaPlayerView::ART_SIZE;
    pixels.resize(n * n);
    uint32_t seed = 12345;
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        seed = seed * 1103515245u + 12345u;
        double grain = ((seed >> 16) & 0xFF) / 255.0 * 14 - 7;
        double t = y / (double)n;
        // Sky: deep blue at the top to orange at the horizon; a sun; two hill silhouettes.
        double r = 40 + 215 * t, g = 40 + 110 * t, b = 120 - 60 * t;
        double sun = std::hypot(x - 120.0, y - 98.0);
        if (sun < 22) {
          r = 255, g = 220 - sun * 2, b = 120;
        } else if (sun < 40) {
          double glow = (40 - sun) / 18;
          r += (255 - r) * glow * 0.6, g += (200 - g) * glow * 0.6;
        }
        double hill1 = 118 + 14 * std::sin(x / 23.0) + 6 * std::sin(x / 7.0);
        double hill2 = 140 + 10 * std::sin(x / 31.0 + 2) + 4 * std::sin(x / 5.0);
        if (y > hill2) {
          r = 20 + (y - hill2) * 0.3, g = 30 + (y - hill2) * 0.5, b = 40;
        } else if (y > hill1) {
          r = 60, g = 50, b = 80;
        }
        pixels[y * n + x] = rgb565(r + grain, g + grain, b + grain);
      }
    }
    image = make_image(pixels, n);
  }
  return &image;
}

static const lv_image_dsc_t *graphic_art() {
  static std::vector<uint16_t> pixels;
  static lv_image_dsc_t image;
  if (pixels.empty()) {
    const int n = MediaPlayerView::ART_SIZE;
    pixels.resize(n * n);
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        // A vertical teal-to-navy gradient with a flat circle and band.
        double t = y / (double)n;
        uint16_t c = rgb565(32 * (1 - t) + 16, 160 * (1 - t) + 32, 168 * (1 - t) + 72);
        if (std::hypot(x - 90.0, y - 80.0) < 46) {
          c = rgb565(248, 200, 64);
        } else if (y > 140 && y < 152) {
          c = rgb565(240, 240, 240);
        }
        pixels[y * n + x] = c;
      }
    }
    image = make_image(pixels, n);
  }
  return &image;
}

// --- Pages -------------------------------------------------------------------------------------

static BootStatus boot;
static void build_boot(lv_obj_t *root, bool landscape) {
  BootScreen::build(root, landscape);
  boot = BootStatus();
  boot.time = "12:34";
  boot.date = "Friday 13 November";
  boot.wifi = "Home WiFi";
  boot.ip = "192.168.1.188";
  boot.mqtt = "MQTT connected";
  boot.loading = "Loading config and status";
  boot.version = "Firmware 0.3.1111";
  BootScreen::show(boot);
}
static void update_boot() {
  boot.loading = "Waiting for the manager";
  BootScreen::show(boot);
}

static HomeState home;
static void fill_home() {
  home = HomeState();
  home.room = "Kitchen";
  home.rooms = {"Kitchen", "Master Bedroom", "Lounge", "Office", "Movie Theatre", "Master Bathroom", "Alex Bedroom",
                "Laundry", "Formal Living", "Foyer", "Garage", "Gym", "Oliver"};
  home.room_index = 0;
  home.time = "12:34 PM";
  home.ceiling = 80;
  home.table = 0;
  home.brightness = 80;
  home.color_temperature = 35;
}
static void build_home(lv_obj_t *root, bool landscape) {
  HomeView::build(root, landscape);
  fill_home();
  HomeView::show(home);
}
static void update_home() {
  home.brightness = home.ceiling = 55;
  HomeView::show(home);
}

// The first dropdown under `obj`, depth first.
static lv_obj_t *find_dropdown(lv_obj_t *obj) {
  if (lv_obj_check_type(obj, &lv_dropdown_class)) {
    return obj;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
    if (lv_obj_t *found = find_dropdown(lv_obj_get_child(obj, i))) {
      return found;
    }
  }
  return NULL;
}
static lv_obj_t *home_root = NULL;
static void build_home_rooms(lv_obj_t *root, bool landscape) {
  home_root = root;
  build_home(root, landscape);
}
static void open_room_list() {
  lv_dropdown_open(find_dropdown(home_root));
}
static void build_home_all(lv_obj_t *root, bool landscape) {
  HomeView::build(root, landscape);
  fill_home();
  home.room = "All rooms";
  home.room_index = -1;
  home.all_rooms = true;
  home.entities = false;
  HomeView::show(home);
}

static EntitiesState entities;
static void slot(int i, const char *name, const char *icon, bool on) {
  entities.slots[i].name = name;
  entities.slots[i].icon = icon;
  entities.slots[i].on = on;
  entities.slots[i].empty = false;
}

static void build_entities4(lv_obj_t *root, bool landscape) {
  EntitiesView::build(root, landscape);
  entities = EntitiesState();
  entities.header = "Kitchen";
  entities.size = 4;
  slot(0, "Ceiling lights", NSPM_ICON_CEILING_LIGHT, true);
  slot(1, "Pendants", NSPM_ICON_LAMPS, true);
  slot(2, "Under cabinet", NSPM_ICON_LIGHTBULB_OUTLINE, false);
  slot(3, "Coffee maker", NSPM_ICON_COFFEE_MAKER, false);
  EntitiesView::show(entities);
}
static void update_toggle_slot() {
  entities.slots[2].on = !entities.slots[2].on;
  EntitiesView::show(entities);
}

static void build_entities8(lv_obj_t *root, bool landscape) {
  EntitiesView::build(root, landscape);
  entities = EntitiesState();
  entities.header = "Lounge";
  entities.size = 8;
  slot(0, "Ceiling", NSPM_ICON_CEILING_LIGHT, true);
  slot(1, "Floor lamp", NSPM_ICON_LAMP, false);
  slot(2, "TV", NSPM_ICON_TELEVISION, true);
  slot(3, "Fan", NSPM_ICON_FAN, false);
  slot(4, "Blinds", NSPM_ICON_WINDOW_SHUTTER_OPEN, false);
  slot(5, "Thermostat", NSPM_ICON_THERMOSTAT, false);
  slot(6, "Sonos", NSPM_ICON_SPEAKER, true);
  slot(7, "Outlet", NSPM_ICON_POWER_SOCKET_US, false);
  EntitiesView::show(entities);
}

static void build_scenes(lv_obj_t *root, bool landscape) {
  EntitiesView::build(root, landscape);
  entities = EntitiesState();
  entities.header = "Hold to save: Evening";
  entities.size = 12;
  const char *names[] = {"Morning", "Daytime", "Evening", "Movie", "Dinner", "Reading",
                         "Party", "Night", "Away", "Cleaning", "Relax", "All off"};
  for (int i = 0; i < 12; i++) {
    slot(i, names[i], i == 11 ? NSPM_ICON_LIGHTBULB_OUTLINE : NSPM_ICON_PALETTE, i == 2);
  }
  entities.save_progress = 60;
  EntitiesView::show(entities);
}
static void update_scenes() {
  entities.save_progress = -1;
  entities.header = "Scene saved";
  EntitiesView::show(entities);
}

static void build_sources(lv_obj_t *root, bool landscape) {
  EntitiesView::build(root, landscape);
  entities = EntitiesState();
  entities.header = "Favorites";
  entities.size = 8;
  entities.can_prev = false;
  const char *names[] = {"Discover Weekly", "Jazz Classics", "KEXP 90.3", "Lo-Fi Beats",
                         "Morning Coffee", "Road Trip", "This Is Radiohead", "Chill Mix"};
  for (int i = 0; i < 8; i++) {
    slot(i, names[i], NULL, i == 2);
  }
  EntitiesView::show(entities);
}
static void update_sources() {
  entities.slots[2].on = false;
  entities.slots[5].on = true;
  EntitiesView::show(entities);
}

static LightState light;
static void build_light_kelvin(lv_obj_t *root, bool landscape) {
  LightView::build(root, landscape);
  light = LightState();
  light.name = "Kitchen pendants";
  light.brightness = 70;
  light.color_temperature = 40;
  LightView::show(light);
}
static void update_light_kelvin() {
  light.color_temperature = 75;
  LightView::show(light);
}

static void build_light_color(lv_obj_t *root, bool landscape) {
  LightView::build(root, landscape);
  light = LightState();
  light.name = "Accent strip";
  light.color_mode = true;
  light.brightness = 60;
  light.hue = 280;
  light.saturation = 80;
  LightView::show(light);
}
static void update_light_color() {
  light.hue = 120;
  LightView::show(light);
}

static ThermostatState thermostat;
static void build_thermostat(lv_obj_t *root, bool landscape) {
  ThermostatView::build(root, landscape);
  thermostat = ThermostatState();
  thermostat.name = "Lounge";
  thermostat.can_page = true;
  thermostat.current = "71.5\xC2\xB0" "F";
  thermostat.set_label = "Heat to";
  thermostat.set_value = "68\xC2\xB0" "F";
  struct {
    const char *name, *value, *icon;
  } options[] = {
      {"Mode", "Heat", NSPM_ICON_FIRE},     {"Fan", "Auto", NSPM_ICON_FAN_AUTO}, {"Preset", "Eco", NSPM_ICON_LEAF},
      {"Swing", "Off", NSPM_ICON_ARROW_OSCILLATING}, {"Speed", "Low", NSPM_ICON_FAN_SPEED_1},
  };
  for (int i = 0; i < 5; i++) {
    thermostat.options[i].name = options[i].name;
    thermostat.options[i].value = options[i].value;
    thermostat.options[i].icon = options[i].icon;
  }
  ThermostatView::show(thermostat);
}
static void update_thermostat() {
  thermostat.set_value = "69\xC2\xB0" "F";
  ThermostatView::show(thermostat);
}

static CoverState cover;
static void build_cover(lv_obj_t *root, bool landscape) {
  CoverView::build(root, landscape);
  cover = CoverState();
  cover.name = "Lounge blinds";
  cover.state = "Open, 75%";
  cover.is_open = true;
  cover.position = 75;
  CoverView::show(cover);
}
static void update_cover() {
  cover.state = "Closing";
  cover.position = 40;
  CoverView::show(cover);
}

static MediaPlayerState media;
static void media_mock(const lv_image_dsc_t *art) {
  media = MediaPlayerState();
  media.name = "James\xE2\x80\x99s Caf\xC3\xA9"; // Glyphs only the Latin fallback fonts have
  media.title = "Everything In Its Right Place";
  media.artist = "Radiohead";
  media.album = "Kid A";
  media.art = art;
  media.playing = true;
  media.volume = 40;
  media.position = 42;
  media.elapsed = "1:42";
  media.duration = "4:11";
  media.has_source = true;
}
static void build_media_photo(lv_obj_t *root, bool landscape) {
  MediaPlayerView::build(root, landscape);
  media_mock(photo_art());
  MediaPlayerView::show(media);
}
static void update_media_volume() {
  media.volume = 45;
  MediaPlayerView::show(media);
}
static void build_media_graphic(lv_obj_t *root, bool landscape) {
  MediaPlayerView::build(root, landscape);
  media_mock(graphic_art());
  media.title = "Teardrop";
  media.artist = "Massive Attack";
  media.album = "Mezzanine";
  MediaPlayerView::show(media);
}
static void update_media_track() {
  // Next track: new text and the photo replacing the graphic cover.
  media.title = "Everything In Its Right Place";
  media.artist = "Radiohead";
  media.album = "Kid A";
  media.art = photo_art();
  media.position = 0;
  media.elapsed = "0:00";
  MediaPlayerView::show(media);
}
static void build_media_noart(lv_obj_t *root, bool landscape) {
  MediaPlayerView::build(root, landscape);
  media_mock(NULL);
  media.title = "KEXP 90.3 FM";
  media.artist = "The Morning Show";
  media.album = "";
  media.position = -1;
  media.controlling_source = true;
  media.can_previous = media.can_next = false;
  MediaPlayerView::show(media);
}
static void update_media_pause() {
  media.playing = false;
  MediaPlayerView::show(media);
}

static ScreensaverState saver;
static void saver_mock() {
  saver = ScreensaverState();
  saver.time = "12:34";
  saver.am_pm = "PM";
  saver.date = "Friday 13 November";
  saver.outside_icon = NSPM_ICON_WEATHER_PARTLY_CLOUDY;
  saver.outside = "54\xC2\xB0" "F";
  saver.inside = "71\xC2\xB0" "F";
  saver.max_min = "61\xC2\xB0 / 47\xC2\xB0";
  saver.rain = "20%";
  saver.wind = "8 mph";
  saver.sunrise = "6:42 AM";
  saver.sunset = "4:51 PM";
  struct {
    const char *day, *icon, *max_min, *rain, *wind;
  } days[] = {
      {"Sat", NSPM_ICON_WEATHER_SUNNY, "58\xC2\xB0/44\xC2\xB0", "0%", "6 mph"},
      {"Sun", NSPM_ICON_WEATHER_PARTLY_CLOUDY, "60\xC2\xB0/46\xC2\xB0", "10%", "5 mph"},
      {"Mon", NSPM_ICON_WEATHER_RAINY, "55\xC2\xB0/45\xC2\xB0", "80%", "12 mph"},
      {"Tue", NSPM_ICON_WEATHER_CLOUDY, "52\xC2\xB0/41\xC2\xB0", "30%", "9 mph"},
      {"Wed", NSPM_ICON_WEATHER_POURING, "50\xC2\xB0/39\xC2\xB0", "60%", "14 mph"},
  };
  for (int i = 0; i < 5; i++) {
    saver.forecast[i].day = days[i].day;
    saver.forecast[i].icon = days[i].icon;
    saver.forecast[i].max_min = days[i].max_min;
    saver.forecast[i].rain = days[i].rain;
    saver.forecast[i].wind = days[i].wind;
  }
}
static void build_saver_weather(lv_obj_t *root, bool landscape) {
  ScreensaverView::build(root, landscape);
  saver_mock();
  ScreensaverView::show(saver);
}
static void update_saver_tick() {
  saver.time = "12:35";
  ScreensaverView::show(saver);
}
static void build_saver_clock(lv_obj_t *root, bool landscape) {
  ScreensaverView::build(root, landscape);
  saver_mock();
  saver.weather = false;
  saver.background = false;
  ScreensaverView::show(saver);
}

static MenuState menu;
static void build_menu(lv_obj_t *root, bool landscape) {
  MenuView::build(root, landscape);
  DialogView::build(root, landscape);
  menu = MenuState();
  menu.items[0] = {NSPM_ICON_SHIELD_HOME, "ON", "Coming soon...", true};
  menu.items[1] = {NSPM_ICON_WEATHER_NIGHT, "ON", "Coming soon...", true};
  menu.items[2] = {NSPM_ICON_SHIELD_OFF, "Disarmed", "Coming soon...", false};
  menu.items[4] = {NSPM_ICON_SPRAY_BOTTLE, "Clean screen", "", false};
  menu.items[5] = {NSPM_ICON_RESTART, "Reboot", "Coming soon...", false};
  MenuView::show(menu);
}
static DialogState reboot_dialog() {
  DialogState dialog;
  dialog.visible = true;
  dialog.title = "Reboot the panel?";
  dialog.message = "It restarts and reconnects to the manager in about 30 seconds.";
  dialog.confirm = "Reboot";
  dialog.danger = true;
  return dialog;
}
static void update_menu_dialog() { DialogView::show(reboot_dialog()); }
static void build_menu_dialog(lv_obj_t *root, bool landscape) {
  build_menu(root, landscape);
  DialogView::show(reboot_dialog());
}
static void update_dialog_close() { DialogView::show(DialogState()); }

static CleanScreenState clean;
static void build_clean(lv_obj_t *root, bool landscape) {
  CleanScreenView::build(root, landscape);
  clean = CleanScreenState();
  clean.remaining = 100;
  CleanScreenView::show(clean);
}
static void update_clean() {
  clean.remaining = 99;
  CleanScreenView::show(clean);
}

const std::vector<PageCase> &page_cases() {
  static const std::vector<PageCase> cases = {
      {"boot", build_boot, "loading text changes", update_boot},
      {"home", build_home, "brightness 80 -> 55", update_home},
      {"home-rooms", build_home_rooms, "room list opens", open_room_list},
      {"home-all", build_home_all, nullptr, nullptr},
      {"entities-4", build_entities4, "toggle a light", update_toggle_slot},
      {"entities-8", build_entities8, "toggle a light", update_toggle_slot},
      {"scenes-12", build_scenes, "scene saved", update_scenes},
      {"media-sources", build_sources, "source chosen", update_sources},
      {"light-temperature", build_light_kelvin, "colour temperature", update_light_kelvin},
      {"light-colour", build_light_color, "hue 280 -> 120", update_light_color},
      {"thermostat", build_thermostat, "setpoint 68 -> 69", update_thermostat},
      {"cover", build_cover, "closing, 75% -> 40%", update_cover},
      {"media-photo-art", build_media_photo, "volume 40 -> 45", update_media_volume},
      {"media-graphic-art", build_media_graphic, "next track, photo art", update_media_track},
      {"media-no-art", build_media_noart, "play -> pause", update_media_pause},
      {"screensaver-weather", build_saver_weather, "clock 12:34 -> 12:35", update_saver_tick},
      {"screensaver-clock", build_saver_clock, "clock 12:34 -> 12:35", update_saver_tick},
      {"menu", build_menu, "reboot dialog opens", update_menu_dialog},
      {"menu-dialog", build_menu_dialog, "dialog closes", update_dialog_close},
      {"clean-screen", build_clean, "countdown tick", update_clean},
  };
  return cases;
}
