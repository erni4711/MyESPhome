import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import p4_camera

# Create a namespace for your component
my_ns = cg.esphome_ns.namespace("screenshot")

# Define the C++ class
Screenshot = my_ns.class_("Screenshot", cg.Component)

CONF_CAMERA_ID = "camera_id"


# Configuration schema
schema = cv.Schema({
    cv.GenerateID(): cv.declare_id(Screenshot),
    cv.Optional(CONF_CAMERA_ID): cv.use_id(p4_camera.P4Camera),
})

CONFIG_SCHEMA = schema.extend(cv.COMPONENT_SCHEMA)

# Code generation
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_CAMERA_ID in config:
        camera = await cg.get_variable(config[CONF_CAMERA_ID])
        cg.add(var.set_camera(camera))
        cg.add_build_flag("-DHAVE_CAMERA=1")
