"""After Dark screensavers on an ESPHome display.

    after_dark:
      display_id: screen         # a pixel display, colour or 1-bit
      # lcd_id: lcd              # or a character LCD (lcd_pcf8574, lcd_gpio)
      savers:                    # what to compile in (default: a few; "all" for all)
        - toasters
        - fishpro:               # some take options, fixed at build time so
            species: [emperor, sardines]   # only what they need goes in
        - clocks:
            face: classic
      saver: toasters            # what to start with, or "off"
      fps: 30                    # default 30, or 8 on a character LCD
      frame_rate_modulation: false  # 1-bit panels: true (2 subframes) or 2-4;
                                    # character LCDs: dim cells (DOS Shell's prompt)
      time_id: sntp_time         # for the clocks (optional)

Saver options (see SAVER_OPTIONS):
  fishpro: species - any of FISH_SPECIES. Default: all nine on a 1-bit panel;
           on a colour one, all but the yellow tang, red clown and jellyfish,
           whose colour art is 110 KB between them.
  clocks:  face - melting_digital, classic, modern, art_deco or mutating.
           Default: mutating (ten photographs, 189 KB) on colour,
           melting_digital on a 1-bit panel.
  dolphins: on a character LCD (each needs time_id):
           clock - true shows the time (HH:MM) in the sky, a dolphin leaping
             in front of it;
           date - true has the date take turns with it, through blank;
           latitude, longitude - degrees, or sensors giving them (Home
             Assistant's zone.home has both as attributes): the real sun
             and moon cross the sky, rising at the right, the moon in its
             phase, and the stars come out only at night;
           weather - a text sensor with Home Assistant's conditions (rainy,
             cloudy...): a glyph at the right of the sky for all but a
             clear one, and no stars when it's clouded over.
  meters:  a pair of VU meters, left and right (an extra, not a module):
           left, right - a sensor, or a lambda read every frame, giving the
             level (right follows left if it has none); with neither, the
             meters dance to made-up music;
           min, max - the level at each end of the scale (default 0-100);
           style - on a character LCD: bars (a row each, to the dot, the
             peak held), needles (two meters in the top row) or both,
             taking turns every half minute. Default bars.
  dosshell: commands - typed instead of DIR and the rest, each answered with
           a line per Home Assistant entity (needs `api:`):
             - dosshell:
                 commands:
                   - name: TEMP
                     entities:
                       - sensor.living_room_temperature
                       - entity_id: sensor.outdoor_temperature
                         label: Outside
                       - entity_id: climate.hall
                         attribute: current_temperature
                         unit: "°C"
                 classic: true    # mix the module's own commands in too

      mode: screensaver          # the display keeps its own lambda until
                                 # after_dark.start; after_dark.stop gives it
                                 # back (saver: random picks a new one each time)
      overlay: |-                # character LCD: lines shown instead while
        return {};               # this returns any (a button's message)

In the default mode (always) the display should have no lambda or pages; this
draws into it directly. In screensaver mode it keeps them.

DOS Shell can also be given a Unix look and lines worked out in lambdas:
  - dosshell:
      style: unix                # prompt, banner and errors to match
      prompt: "loft$ "           # $P directory, $G >, $T time, $D date
      banner: ["Linux loft 6.6.51"]
      bad_command: "%s: command not found"
      commands:
        - name: weather
          lines:
            - lambda: 'return "Out " + to_string(id(outside).state);'
        - name: cal
          lambda: 'return id(event_titles);'   # std::vector<std::string>
        - name: bins
          when: 'return id(bins_due);'           # typed only while true
          lines: [...]
"""
import inspect
from pathlib import Path
import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import display, sensor, text_sensor
from esphome.const import CONF_DIMENSIONS, CONF_ID, CONF_TIME_ID
from esphome.core import CORE, ID

CODEOWNERS = ["@Vortitron"]

CONF_DISPLAY_ID = "display_id"
CONF_LCD_ID = "lcd_id"
CONF_SAVERS = "savers"
CONF_SAVER = "saver"
CONF_FPS = "fps"
CONF_FRM = "frame_rate_modulation"
CONF_AFTER_DARK_ID = "after_dark_id"
CONF_MODE = "mode"
CONF_OVERLAY = "overlay"
CLOCKS_FACES = ["melting_digital", "classic", "modern", "art_deco", "mutating"]
FISH_SPECIES = [
    "yellow_tang", "red_clown", "blue_tanger", "trigger_fish", "pink_squirrel",
    "butterfly_fish", "emperor", "jellyfish", "sardines",
]
FISH_COLOUR_DEFAULT = [s for s in FISH_SPECIES if s not in ("yellow_tang", "red_clown", "jellyfish")]



def _find_savers():
    """The savers in this directory, from each file's AD_REGISTER_SAVER line:
    name -> {"text": whether it has a character LCD version}. Each is built
    only when asked for, behind -DAD_SAVER_<NAME>."""
    found = {}
    pattern = re.compile(r'^AD_REGISTER_SAVER\("(\w+)",\s*(\w+),\s*(\w+)\)', re.M)
    for path in sorted(Path(__file__).parent.glob("ad_*.cpp")):
        for name, make, make_text in pattern.findall(path.read_text()):
            found[name] = {"text": make_text != "nullptr", "make": make}
    return dict(sorted(found.items()))


SAVERS = _find_savers()
# Built when a config lists none: a handful that suit every kind of screen,
# character LCDs included, and leave room for the rest of the firmware.
# `savers: all` builds every one.
DEFAULT_SAVERS = ["toasters", "warp", "flocks", "starry_night", "dolphins"]

after_dark_ns = cg.esphome_ns.namespace("after_dark")
AfterDarkComponent = after_dark_ns.class_("AfterDarkComponent", cg.Component)
StartAction = after_dark_ns.class_("StartAction", automation.Action)
StopAction = after_dark_ns.class_("StopAction", automation.Action)
IsRunningCondition = after_dark_ns.class_("IsRunningCondition", automation.Condition)
LCDDisplay = cg.esphome_ns.namespace("lcd_base").class_("LCDDisplay", cg.PollingComponent)
RealTimeClock = cg.esphome_ns.namespace("time").class_("RealTimeClock", cg.PollingComponent)


def _shell_entity(value):
    """A line of a DOS Shell command: an entity's id, the id with a label,
    attribute or unit of its own, or a lambda returning the whole line."""
    if isinstance(value, str):
        value = {"entity_id": value}
    if isinstance(value, dict) and "lambda" in value:
        return cv.Schema({cv.Required("lambda"): cv.returning_lambda})(value)
    return cv.Schema(
        {
            cv.Required("entity_id"): cv.entity_id,
            cv.Optional("label"): cv.string,
            cv.Optional("attribute"): cv.string,
            cv.Optional("unit"): cv.string,
        }
    )(value)


# Build-time options, under a saver's name in `savers:`. Most become -D flags
# in to_code, so the saver compiles in only the art it needs.
SAVER_OPTIONS = {
    "fishpro": cv.Schema(
        {
            cv.Optional("species"): cv.All(
                cv.ensure_list(cv.one_of(*FISH_SPECIES, lower=True, space="_")), cv.Length(min=1)
            ),
        }
    ),
    "clocks": cv.Schema({cv.Optional("face"): cv.one_of(*CLOCKS_FACES, lower=True, space="_")}),
    "dolphins": cv.Schema(
        {
            cv.Optional("clock", default=False): cv.boolean,
            cv.Optional("date", default=False): cv.boolean,
            cv.Optional("latitude"): cv.Any(cv.float_range(min=-90, max=90), cv.use_id(sensor.Sensor)),
            cv.Optional("longitude"): cv.Any(cv.float_range(min=-180, max=360), cv.use_id(sensor.Sensor)),
            cv.Optional("weather"): cv.use_id(text_sensor.TextSensor),
        }
    ),
    "meters": cv.Schema(
        {
            cv.Optional("left"): cv.Any(cv.use_id(sensor.Sensor), cv.returning_lambda),
            cv.Optional("right"): cv.Any(cv.use_id(sensor.Sensor), cv.returning_lambda),
            cv.Optional("min", default=0): cv.float_,
            cv.Optional("max", default=100): cv.float_,
            cv.Optional("style", default="bars"): cv.one_of("bars", "needles", "both", lower=True),
        }
    ),
    "dosshell": cv.Schema(
        {
            cv.Optional("commands"): cv.All(
                cv.ensure_list(
                    cv.All(
                        cv.Schema(
                            {
                                cv.Required("name"): cv.All(cv.string_strict, cv.Length(min=1, max=12)),
                                cv.Optional("lines"): cv.All(cv.ensure_list(_shell_entity), cv.Length(min=1)),
                                cv.Optional("entities"): cv.All(cv.ensure_list(_shell_entity), cv.Length(min=1)),
                                cv.Optional("lambda"): cv.returning_lambda,
                                cv.Optional("when"): cv.returning_lambda,
                            }
                        ),
                        cv.has_at_least_one_key("lines", "entities", "lambda"),
                    )
                ),
                cv.Length(min=1),
            ),
            cv.Optional("classic"): cv.boolean,
            cv.Optional("style", default="dos"): cv.one_of("dos", "unix", lower=True),
            cv.Optional("prompt"): cv.string,
            cv.Optional("banner"): cv.ensure_list(cv.string),
            cv.Optional("bad_command"): cv.string,
        }
    ),
}


def _savers(value):
    """A list of saver names, each optionally with its options under it, made
    into {name: options}. "all" (alone, or in the list beside savers given
    options) adds every saver not otherwise listed."""
    out = {}
    everything = False
    for item in cv.ensure_list(cv.Any(cv.string, dict))(value):
        if isinstance(item, str) and item.lower() == "all":
            everything = True
            continue
        if isinstance(item, dict):
            if len(item) != 1:
                raise cv.Invalid("give one saver per list item, with its options indented under it")
            name, options = next(iter(item.items()))
        else:
            name, options = item, None
        name = cv.one_of(*SAVERS, lower=True)(name)
        if name in out:
            raise cv.Invalid(f"'{name}' is listed twice")
        schema = SAVER_OPTIONS.get(name)
        if options and schema is None:
            raise cv.Invalid(f"'{name}' has no options")
        out[name] = schema(options or {}) if schema else {}
    if everything:
        for name in SAVERS:
            if name not in out:
                out[name] = SAVER_OPTIONS[name]({}) if name in SAVER_OPTIONS else {}
        out = {name: out[name] for name in SAVERS}
    if not out:
        raise cv.Invalid("list at least one saver")
    return out


def available_savers(config):
    """The savers this config can show, in order."""
    savers = list(config[CONF_SAVERS])
    if CONF_LCD_ID in config:
        savers = [s for s in savers if SAVERS[s]["text"]]
    return savers


def _subframes(value):
    """false, true (2 subframes), or 2-4."""
    if isinstance(value, bool) or (isinstance(value, str) and not value.strip().isdigit()):
        return 2 if cv.boolean(value) else 1
    return cv.int_range(min=2, max=4)(value)


def _check(config):
    screensaver = config[CONF_MODE] == "screensaver"
    if screensaver and CONF_LCD_ID in config:
        raise cv.Invalid("screensaver mode is for pixel displays (display_id)", [CONF_MODE])
    if CONF_OVERLAY in config and CONF_LCD_ID not in config:
        raise cv.Invalid("overlay is for character LCDs (lcd_id)", [CONF_OVERLAY])
    if screensaver:
        saver = config.get(CONF_SAVER, "random")
        if saver not in ("random", "off") and saver not in available_savers(config):
            raise cv.Invalid(f"'{saver}' isn't built: {available_savers(config)}", [CONF_SAVER])
        config[CONF_SAVER] = saver
        return config
    if CONF_LCD_ID in config and not available_savers(config):
        raise cv.Invalid("none of these savers has a character LCD version", [CONF_SAVERS])
    saver = config.get(CONF_SAVER, available_savers(config)[0])
    if saver != "off" and saver not in available_savers(config):
        raise cv.Invalid(f"'{saver}' isn't built for this screen: {available_savers(config)}", [CONF_SAVER])
    config[CONF_SAVER] = saver
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AfterDarkComponent),
            cv.Exclusive(CONF_DISPLAY_ID, "screen"): cv.use_id(display.Display),
            cv.Exclusive(CONF_LCD_ID, "screen"): cv.use_id(LCDDisplay),
            cv.Optional(CONF_SAVERS, default=DEFAULT_SAVERS): _savers,
            cv.Optional(CONF_SAVER): cv.one_of(*SAVERS, "off", "random", lower=True),
            cv.Optional(CONF_MODE, default="always"): cv.one_of("always", "screensaver", lower=True),
            cv.Optional(CONF_OVERLAY): cv.returning_lambda,
            cv.Optional(CONF_FPS): cv.int_range(min=1, max=60),
            cv.Optional(CONF_FRM, default=False): _subframes,
            cv.Optional(CONF_TIME_ID): cv.use_id(RealTimeClock),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_exactly_one_key(CONF_DISPLAY_ID, CONF_LCD_ID),
    _check,
)


# Display platforms that are 1-bit, so the colour artwork can stay out.
MONO_PLATFORMS = {
    "ssd1306_i2c", "ssd1306_spi", "ssd1305_i2c", "ssd1305_spi", "st7567_i2c", "st7567_spi",
    "pcd8544", "st7920", "max7219digit", "waveshare_epaper", "inkplate6",
}


def _display_conf(display_id):
    for conf in CORE.config.get("display", []):
        if conf.get(CONF_ID) == display_id:
            return conf
    raise cv.Invalid(f"can't find display '{display_id}'")


def _final_validate(config):
    shell = config[CONF_SAVERS].get("dosshell") or {}
    if shell.get("commands") and "api" not in fv.full_config.get():
        raise cv.Invalid("DOS Shell's commands get their data from Home Assistant through `api:`; add it")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


# What `style: unix` means, unless the options say otherwise.
def _unix_style(name):
    return {
        "prompt": f"{name}$ ",
        "banner": [f"Linux {name} 6.6.51 armv6l", "", f"{name} login: pi", "Last login: on ttyS0"],
        "bad_command": "%s: command not found",
    }


async def _shell_to_code(var, options):
    unix = options["style"] == "unix"
    style = _unix_style(CORE.name) if unix else {}
    for key in ("prompt", "banner", "bad_command"):
        if key in options:
            style[key] = options[key]
    if "prompt" in style:
        cg.add(var.set_shell_prompt(style["prompt"]))
    for line in style.get("banner", []):
        cg.add(var.add_shell_banner(line))
    if "bad_command" in style:
        cg.add(var.set_shell_bad_command(style["bad_command"]))
    if unix:
        cg.add(var.set_shell_lowercase(True))
    cg.add(var.set_shell_classic(options.get("classic", not unix and not options.get("commands"))))
    for command in options.get("commands", []):
        cg.add(var.add_shell_command(command["name"]))
        for line in command.get("lines", []) + command.get("entities", []):
            if "lambda" in line:
                text = await cg.process_lambda(line["lambda"], [], return_type=cg.std_string)
                cg.add(var.add_shell_text(text))
            else:
                cg.add(var.add_shell_entity(line["entity_id"], line.get("attribute", ""),
                                            line.get("label", ""), line.get("unit", "")))
        if "lambda" in command:
            lines = await cg.process_lambda(command["lambda"], [], return_type=cg.std_vector.template(cg.std_string))
            cg.add(var.set_shell_lines(lines))
        if "when" in command:
            when = await cg.process_lambda(command["when"], [], return_type=cg.bool_)
            cg.add(var.set_shell_when(when))


AFTER_DARK_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(AfterDarkComponent)})

# Both actions finish inside play(). ESPHome 2026 wants that said; earlier
# releases have no such argument.
_SYNC = {"synchronous": True} if "synchronous" in inspect.signature(automation.register_action).parameters else {}


@automation.register_action("after_dark.start", StartAction, AFTER_DARK_ACTION_SCHEMA, **_SYNC)
@automation.register_action("after_dark.stop", StopAction, AFTER_DARK_ACTION_SCHEMA, **_SYNC)
async def after_dark_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("after_dark.is_running", IsRunningCondition, AFTER_DARK_ACTION_SCHEMA)
async def after_dark_condition_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def _dolphins_to_code(var, options):
    """The dolphins' sky on a character LCD: the clock, and the real sun, moon
    and weather from the YAML's sensors."""
    for flag in ("clock", "date"):
        if options.get(flag):
            cg.add_global(cg.RawStatement(f"namespace adcore {{ extern bool dolphins_{flag}; }}"))
            cg.add(cg.RawExpression(f"::adcore::dolphins_{flag} = true"))
    for key in ("latitude", "longitude"):
        if key not in options:
            continue
        setter = f"set_sky_{key}"
        if isinstance(options[key], ID):
            source = await cg.get_variable(options[key])
            cg.add(source.add_on_state_callback(cg.RawExpression(f"[](float x) {{ {var}->{setter}(x); }}")))
        else:
            cg.add(getattr(var, setter)(options[key]))
    if "weather" in options:
        source = await cg.get_variable(options["weather"])
        cg.add(
            source.add_on_state_callback(
                cg.RawExpression(f"[](const std::string &s) {{ {var}->set_sky_weather(s); }}")
            )
        )


async def _meters_to_code(var, options):
    """What drives the meters: a sensor's state or a lambda per channel."""
    inp = "::adcore::meter_input"
    cg.add(cg.RawExpression(f"{inp}.lo = {options['min']}f"))
    cg.add(cg.RawExpression(f"{inp}.hi = {options['max']}f"))
    style = ["bars", "needles", "both"].index(options["style"])
    if style:
        cg.add(cg.RawExpression(f"{inp}.style = {style}"))
    for ch, key in enumerate(("left", "right")):
        if key not in options:
            continue
        if isinstance(options[key], ID):
            source = await cg.get_variable(options[key])
            cg.add(source.add_on_state_callback(cg.RawExpression(f"[](float x) {{ {inp}.set({ch}, x); }}")))
        else:
            fn = await cg.process_lambda(options[key], [], return_type=cg.float_)
            cg.add(cg.RawExpression(f"{inp}.fn[{ch}] = {fn}"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mono = False
    if CONF_LCD_ID in config:
        cg.add_build_flag("-DAD_USE_LCD")
        lcd = await cg.get_variable(config[CONF_LCD_ID])
        # Pixel savers and their art are dropped (AD_TEXT_ONLY).
        cg.add_build_flag("-DAD_TEXT_ONLY")
        cols, rows = _display_conf(config[CONF_LCD_ID])[CONF_DIMENSIONS]
        cg.add(var.set_lcd(lcd, cols, rows))
        fps = config.get(CONF_FPS, 8)
    else:
        mono = _display_conf(config[CONF_DISPLAY_ID]).get("platform") in MONO_PLATFORMS
        if mono:
            cg.add_build_flag("-DAD_MONO_ONLY")  # savers leave their colour art out
        else:
            cg.add_build_flag("-DAD_COLOUR_ONLY")  # and these their 1-bit art
        disp = await cg.get_variable(config[CONF_DISPLAY_ID])
        cg.add(var.set_display(disp))
        fps = config.get(CONF_FPS, 30)
    cg.add(var.set_fps(fps))
    if config[CONF_FRM] > 1:
        cg.add(var.set_frame_rate_modulation(config[CONF_FRM]))
    if CONF_TIME_ID in config:
        cg.add_build_flag("-DAD_USE_TIME")
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_initial_saver(config[CONF_SAVER]))
    if config[CONF_MODE] == "screensaver":
        cg.add(var.set_screensaver_mode(True))
    if CONF_OVERLAY in config:
        overlay = await cg.process_lambda(
            config[CONF_OVERLAY], [], return_type=cg.std_vector.template(cg.std_string)
        )
        cg.add(var.set_overlay(overlay))
    for saver, options in config[CONF_SAVERS].items():
        cg.add_build_flag(f"-DAD_SAVER_{saver.upper()}")
        # Refer to the saver, or an ESP-IDF build's linker leaves it out.
        make = SAVERS[saver]["make"]
        cg.add_global(cg.RawStatement(f"namespace adcore {{ void ad_link_{make}(); }}"))
        cg.add(cg.RawExpression(f"::adcore::ad_link_{make}()"))
        if saver == "fishpro":
            species = options.get("species", FISH_SPECIES if mono else FISH_COLOUR_DEFAULT)
            cg.add_build_flag("-DAD_FISHPRO_PICKED")
            for fish in species:
                cg.add_build_flag(f"-DAD_FISHPRO_{fish.upper()}")
        elif saver == "clocks" and "face" in options:
            cg.add_build_flag(f"-DAD_CLOCKS_TYPE={CLOCKS_FACES.index(options['face'])}")
        elif saver == "dolphins":
            await _dolphins_to_code(var, options)
        elif saver == "meters":
            await _meters_to_code(var, options)
        elif saver == "dosshell":
            if options.get("commands"):
                cg.add_build_flag("-DAD_SHELL_LIVE")
                cg.add_define("USE_API_HOMEASSISTANT_STATES")
            await _shell_to_code(var, options)
