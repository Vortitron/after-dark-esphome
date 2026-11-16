"""Frame-rate modulation on a 1-bit panel as a switch, to judge the shimmer
for yourself. It starts as the YAML says, and is kept over a restart once
changed unless restore_mode says otherwise.

    switch:
      - platform: after_dark
        name: Screensaver shading
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_AFTER_DARK_ID, AfterDarkComponent, after_dark_ns

DEPENDENCIES = ["after_dark"]

AfterDarkSwitch = after_dark_ns.class_("AfterDarkSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = (
    switch.switch_schema(
        AfterDarkSwitch,
        icon="mdi:gradient-horizontal",
        entity_category=ENTITY_CATEGORY_CONFIG,
        block_inverted=True,
        # Kept over a restart once changed; until then, as the YAML says.
        default_restore_mode="RESTORE_DEFAULT_OFF",
    )
    .extend({cv.GenerateID(CONF_AFTER_DARK_ID): cv.use_id(AfterDarkComponent)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AFTER_DARK_ID])
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
