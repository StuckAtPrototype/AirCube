"""StuckAtPrototype AirCube air quality monitor quirk for ZHA.

Firmware v1.3 sends eCO2, eTVOC, and AQI on custom cluster 0xFC01 as
NON-manufacturer-specific attributes (manuf_code = 0xFFFF sentinel in the
report_attr_cmd_req frame, and the cluster is registered without the
ESP_ZB_ZCL_ATTR_MANUF_SPEC access bit).

The previous version of this quirk declared is_manufacturer_specific=True,
which made zigpy expect a manufacturer code in the incoming ZCL frame
header and silently drop the reports as malformed. Setting it to False
matches how the firmware actually frames the reports and lets
_update_attribute fire as intended.

Verified by reverse-engineering AirCube_v1_3_2MB_final.bin:
  - aircube_zb_publish at 0x42010f34 calls esp_zb_zcl_set_attribute_val
    five times per sample (temp, hum, eco2, etvoc, aqi) with identical
    call shape; only cluster/attribute IDs differ.
  - Manual report_attr_cmd_req wrapper at 0x42010ab6 builds the report
    struct with manuf_code = 0xFFFF (sp+0x1c), cluster = 0xFC01, ep = 10.
  - Attribute IDs in the firmware: eco2=0, etvoc=1, aqi=2.
"""

from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.foundation import ZCLAttributeDef
import zigpy.types as t

from homeassistant.components.sensor import SensorDeviceClass, SensorStateClass


class AirQualityCluster(CustomCluster):
    """AirCube custom air quality cluster (0xFC01).

    Although 0xFC01 is in the manufacturer-specific cluster ID range, the
    firmware sends attribute reports on it as ordinary (non-manufacturer-
    specific) ZCL frames. Attribute defs therefore use
    is_manufacturer_specific=False so zigpy parses the frames correctly.
    """

    cluster_id = 0xFC01
    name = "AirCube Air Quality"
    ep_attribute = "aircube_air_quality"

    class AttributeDefs(CustomCluster.AttributeDefs):
        eco2 = ZCLAttributeDef(
            id=0x0000,
            type=t.uint16_t,
            is_manufacturer_specific=False,
        )
        etvoc = ZCLAttributeDef(
            id=0x0001,
            type=t.uint16_t,
            is_manufacturer_specific=False,
        )
        aqi = ZCLAttributeDef(
            id=0x0002,
            type=t.uint16_t,
            is_manufacturer_specific=False,
        )

    def _update_attribute(self, attrid, value):
        """Forward attribute reports so the v2 sensor bindings see updates."""
        super()._update_attribute(attrid, value)
        self.listener_event("attribute_updated", attrid, value)


(
    QuirkBuilder("StuckAtPrototype", "AirCube")
    .replaces(AirQualityCluster, endpoint_id=10)
    .sensor(
        AirQualityCluster.AttributeDefs.eco2.name,
        AirQualityCluster.cluster_id,
        endpoint_id=10,
        unit="ppm",
        device_class=SensorDeviceClass.CO2,
        state_class=SensorStateClass.MEASUREMENT,
        fallback_name="eCO2",
    )
    .sensor(
        AirQualityCluster.AttributeDefs.etvoc.name,
        AirQualityCluster.cluster_id,
        endpoint_id=10,
        unit="ppb",
        device_class=SensorDeviceClass.VOLATILE_ORGANIC_COMPOUNDS_PARTS,
        state_class=SensorStateClass.MEASUREMENT,
        fallback_name="eTVOC",
    )
    .sensor(
        AirQualityCluster.AttributeDefs.aqi.name,
        AirQualityCluster.cluster_id,
        endpoint_id=10,
        device_class=SensorDeviceClass.AQI,
        state_class=SensorStateClass.MEASUREMENT,
        fallback_name="AQI",
    )
    .add_to_registry()
)
