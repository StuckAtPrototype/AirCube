-- Copyright 2026 SmartThings Community contributors
-- Copyright 2026 StuckAtPrototype contributors
-- SPDX-License-Identifier: Apache-2.0
--
-- AirCube (StuckAtPrototype) — custom cluster 0xFC01 for eCO2, eTVOC, AQI.
-- Reference: https://github.com/StuckAtPrototype/AirCube

local capabilities = require "st.capabilities"
local ZigbeeDriver = require "st.zigbee"
local defaults = require "st.zigbee.defaults"
local clusters = require "st.zigbee.zcl.clusters"
local data_types = require "st.zigbee.data_types"
local device_management = require "st.zigbee.device_management"
local cluster_base = require "st.zigbee.cluster_base"

local TemperatureMeasurement = clusters.TemperatureMeasurement
local RelativeHumidity = clusters.RelativeHumidity

--- ZCL data type ID for unsigned 16-bit (same as RelativeHumidity measured value).
local UINT16_ZCL_TYPE = RelativeHumidity.attributes.MeasuredValue.base_type.ID

--- AirCube exposes sensor clusters on endpoint 10 (firmware / ZHA quirk).
local AIRCUBE_ENDPOINT = 0x0A

--- Custom air quality cluster (same as ZHA quirk / Zigbee2MQTT converter).
local AIRCUBE_AQ_CLUSTER = 0xFC01
local ATTR_ECO2 = 0x0000
local ATTR_ETVOC = 0x0001
local ATTR_AQI = 0x0002

--- Reporting for 0xFC01 (intervals and deltas aligned with AirCube / Z2M example).
local AIRCUBE_AQ_REPORTING = {
  { attribute = ATTR_ECO2, min_rep = 30, max_rep = 600, rep_change = 50 },
  { attribute = ATTR_ETVOC, min_rep = 30, max_rep = 600, rep_change = 1 },
  { attribute = ATTR_AQI, min_rep = 30, max_rep = 600, rep_change = 5 },
}

local function emit_eco2(driver, device, value, zb_rx)
  if value.value ~= nil and value.value < 65535 then
    device:emit_event(capabilities.carbonDioxideMeasurement.carbonDioxide({ value = value.value, unit = "ppm" }))
  end
end

local function emit_etvoc(driver, device, value, zb_rx)
  if value.value ~= nil and value.value < 65535 then
    device:emit_event(capabilities.tvocMeasurement.tvocLevel({ value = value.value, unit = "ppb" }))
  end
end

local function emit_aqi(driver, device, value, zb_rx)
  if value.value ~= nil and value.value < 65535 then
    -- ENS16x IAQI is 1–5; airQualitySensor uses the same 1–5 index on SmartThings
    device:emit_event(capabilities.airQualitySensor.airQuality({ value = value.value }))
  end
end

local function aircube_refresh(driver, device)
  device:refresh()
end

local function aircube_init(driver, device)
  for _, cfg in ipairs(AIRCUBE_AQ_REPORTING) do
    device:add_configured_attribute({
      cluster = AIRCUBE_AQ_CLUSTER,
      attribute = cfg.attribute,
      minimum_interval = cfg.min_rep,
      maximum_interval = cfg.max_rep,
      data_type = data_types.Uint16,
      reportable_change = cfg.rep_change,
    })
  end
end

local function aircube_configure(driver, device)
  device:configure()

  -- Bind air-quality cluster to the hub on the correct endpoint.
  device:send(
    device_management.build_bind_request(
      device,
      AIRCUBE_AQ_CLUSTER,
      driver.environment_info.hub_zigbee_eui,
      AIRCUBE_ENDPOINT
    )
  )

  -- Bind standard measurement clusters (matches Zigbee2MQTT configure in AirCube docs).
  device:send(
    device_management.build_bind_request(
      device,
      TemperatureMeasurement.ID,
      driver.environment_info.hub_zigbee_eui,
      AIRCUBE_ENDPOINT
    )
  )
  device:send(
    device_management.build_bind_request(
      device,
      RelativeHumidity.ID,
      driver.environment_info.hub_zigbee_eui,
      AIRCUBE_ENDPOINT
    )
  )

  for _, cfg in ipairs(AIRCUBE_AQ_REPORTING) do
    device:send(
      cluster_base.configure_reporting(
        device,
        data_types.ClusterId(AIRCUBE_AQ_CLUSTER),
        cfg.attribute,
        UINT16_ZCL_TYPE,
        cfg.min_rep,
        cfg.max_rep,
        cfg.rep_change
      ):to_endpoint(AIRCUBE_ENDPOINT)
    )
  end

  -- Ensure temp/humidity reports use endpoint 10 (Z2M-style intervals).
  device:send(
    TemperatureMeasurement.attributes.MeasuredValue:configure_reporting(device, 1, 60, 50):to_endpoint(AIRCUBE_ENDPOINT)
  )
  device:send(
    RelativeHumidity.attributes.MeasuredValue:configure_reporting(device, 1, 60, 100):to_endpoint(AIRCUBE_ENDPOINT)
  )

  device.thread:call_with_delay(2, function()
    device:send(TemperatureMeasurement.attributes.MeasuredValue:read(device):to_endpoint(AIRCUBE_ENDPOINT))
    device:send(RelativeHumidity.attributes.MeasuredValue:read(device):to_endpoint(AIRCUBE_ENDPOINT))
  end)
end

local aircube_driver_template = {
  supported_capabilities = {
    capabilities.temperatureMeasurement,
    capabilities.relativeHumidityMeasurement,
  },
  lifecycle_handlers = {
    init = aircube_init,
    doConfigure = aircube_configure,
  },
  capability_handlers = {
    [capabilities.refresh.ID] = {
      [capabilities.refresh.commands.refresh.NAME] = aircube_refresh,
    },
  },
  zigbee_handlers = {
    attr = {
      [AIRCUBE_AQ_CLUSTER] = {
        [ATTR_ECO2] = emit_eco2,
        [ATTR_ETVOC] = emit_etvoc,
        [ATTR_AQI] = emit_aqi,
      },
    },
  },
  health_check = false, -- device reports on its own schedule; hub polling not needed
}

defaults.register_for_default_handlers(
  aircube_driver_template,
  aircube_driver_template.supported_capabilities,
  { native_capability_attrs_enabled = true }
)

local driver = ZigbeeDriver("aircube-zigbee", aircube_driver_template)
driver:run()
