local APP = {
  DIR = "/sd/apps/classicube",
  MODULE = "/sd/apps/classicube/modules/classicube.so",
  SOURCE = "ble-main",
  POLL_MS = 32,
  TRIGGER_PRESS = 32768,
  TRIGGER_RELEASE = 16384,
  ACTIVITY_DEADZONE = 4000,
}

if app and app.current then
  local current = app.current()
  local entry = current and current.entry
  local dir = type(entry) == "string" and entry:gsub("\\", "/"):match("^(.*)/[^/]+$") or nil
  if dir and dir ~= "" then
    APP.DIR = dir
    APP.MODULE = dir .. "/modules/classicube.so"
  end
end

local function log(...)
  print("[classicube]", ...)
end

local function bit_set(mask, bit)
  mask = tonumber(mask) or 0
  return bit > 0 and math.floor(mask / bit) % 2 >= 1
end

local function raw_buttons(state)
  if type(state) ~= "table" then return 0 end
  return tonumber(state.buttons or state.buttons_mask or state.mask or state.button_mask) or 0
end

local function connected(state)
  if type(state) ~= "table" then return false end
  if state.connected == nil then return true end
  return state.connected == true or state.connected == 1 or state.connected == "true"
end

if app and app.set_home_exit then pcall(function() app.set_home_exit(true) end) end

local function ensure_hidpad_service()
  if not app or not app.start_service then return end
  local running = false
  if app.services then
    pcall(function()
      for _, service in ipairs(app.services() or {}) do
        if service.id == "hidpad" and service.running == true then running = true; break end
      end
    end)
  end
  if not running then
    local ok, err = app.start_service("hidpad")
    if not ok then log("hidpad service unavailable", tostring(err)) end
  end
end

ensure_hidpad_service()

local ok_module, cube = pcall(function() return require(APP.MODULE) end)
if not ok_module or type(cube) ~= "table" then
  log("module load failed", tostring(cube))
  return
end

local started, start_error = cube.start({ root = APP.DIR })
if not started then
  log("start failed", tostring(start_error))
  return
end

local current = { mask = -1, lx = 0, ly = 0, rx = 0, ry = 0 }
local triggers = { lt = false, rt = false }
local controller_trace = { connected = nil, activity = false }
local exiting = false
local poll_timer
local exit_timer

local function stop_timer(timer)
  if not timer then return end
  pcall(function() timer:stop() end)
  pcall(function() timer:unregister() end)
end

local function finish_exit()
  stop_timer(poll_timer); stop_timer(exit_timer)
  if controller and controller.on then pcall(function() controller.on(APP.SOURCE, nil) end) end
  if app and app.exit then pcall(function() app.exit() end) end
end

local function request_exit(reason)
  if exiting then return end
  exiting = true
  log("exit", reason or "")
  pcall(function() cube.set_input(0, 0, 0, 0, 0) end)
  pcall(function() cube.stop() end)
  if tmr and tmr.create then
    exit_timer = tmr.create()
    if pcall(function() exit_timer:alarm(350, tmr.ALARM_SINGLE or 0, finish_exit) end) then return end
  end
  finish_exit()
end

local function map_state(state)
  if not connected(state) then
    triggers.lt, triggers.rt = false, false
    return 0, 0, 0, 0, 0, false
  end
  local raw = raw_buttons(state)
  local mask = 0
  local function set_module(module_bit)
    if module_bit and module_bit > 0 and not bit_set(mask, module_bit) then
      mask = mask + module_bit
    end
  end
  local function add(raw_bit, module_bit, named)
    if bit_set(raw, raw_bit) or named then set_module(module_bit) end
  end
  local function trigger_active(was_active, value)
    if value == true or value == "true" then return true end
    if value == false or value == "false" or value == nil then return false end
    value = tonumber(value) or 0
    if value == 1 then value = 65535 end
    if was_active then return value >= APP.TRIGGER_RELEASE end
    return value >= APP.TRIGGER_PRESS
  end
  add(1, cube.BTN_UP, state.up == true)
  add(2, cube.BTN_DOWN, state.down == true)
  add(4, cube.BTN_LEFT, state.left == true)
  add(8, cube.BTN_RIGHT, state.right == true)
  add(16, cube.BTN_A, state.a == true)
  add(32, cube.BTN_SELECT, state.b == true)
  add(64, cube.BTN_X, state.x == true)
  add(128, cube.BTN_Y, state.y == true)
  add(256, cube.BTN_L, state.l == true or state.lb == true)
  add(512, cube.BTN_R, state.r == true or state.rb == true)
  add(1024, cube.BTN_L2, state.ls == true or state.l3 == true or state.lstick == true)
  add(2048, cube.BTN_R2, state.rs == true or state.r3 == true or state.rstick == true)
  add(4096, cube.BTN_SELECT, state.select == true or state.back == true)
  add(8192, cube.BTN_START, state.start == true or state.menu == true)
  triggers.lt = trigger_active(triggers.lt, state.lt or state.l2)
  triggers.rt = trigger_active(triggers.rt, state.rt or state.r2)
  if triggers.lt then set_module(cube.BTN_R) end
  if triggers.rt then set_module(cube.BTN_L) end
  local home = bit_set(raw, 32768) or state.home == true or state.guide == true
  return mask, tonumber(state.lx) or 0, tonumber(state.ly) or 0,
         tonumber(state.rx) or 0, tonumber(state.ry) or 0, home
end

local function update_input(event_state)
  if exiting then return end
  local state = type(event_state) == "table" and event_state or nil
  if not state and controller and controller.state then
    local ok, value = pcall(function() return controller.state(APP.SOURCE) end)
    if ok then state = value end
  end
  local is_connected = connected(state)
  if controller_trace.connected ~= is_connected then
    controller_trace.connected = is_connected
    controller_trace.activity = false
    if is_connected then
      log("controller connected", tostring(state.name or state.device_name or state.profile or APP.SOURCE))
    else
      log("controller disconnected", APP.SOURCE)
    end
  end
  local mask, lx, ly, rx, ry, home = map_state(state)
  if home then request_exit("controller HOME"); return end
  if is_connected and not controller_trace.activity and
     (mask ~= 0 or math.abs(lx) > APP.ACTIVITY_DEADZONE or
      math.abs(ly) > APP.ACTIVITY_DEADZONE or math.abs(rx) > APP.ACTIVITY_DEADZONE or
      math.abs(ry) > APP.ACTIVITY_DEADZONE) then
    controller_trace.activity = true
    log("controller input", string.format("mask=0x%X lx=%d ly=%d rx=%d ry=%d lt=%s rt=%s",
        mask, lx, ly, rx, ry, tostring(state.lt or state.l2 or 0), tostring(state.rt or state.r2 or 0)))
  end
  if mask ~= current.mask or lx ~= current.lx or ly ~= current.ly or
     rx ~= current.rx or ry ~= current.ry then
    local ok, err = pcall(function() cube.set_input(mask, lx, ly, rx, ry) end)
    if not ok then log("input failed", tostring(err)) end
    current.mask, current.lx, current.ly, current.rx, current.ry = mask, lx, ly, rx, ry
  end
end

if controller and controller.on then
  local ok_subscribe, subscribe_error = pcall(function()
    controller.on(APP.SOURCE, function(source, state)
      if type(state) ~= "table" and type(source) == "table" then state = source end
      update_input(state)
    end)
  end)
  if ok_subscribe then
    log("controller subscribed", APP.SOURCE)
  else
    log("controller subscribe failed", tostring(subscribe_error))
  end
else
  log("controller API unavailable")
end
update_input()

if tmr and tmr.create then
  poll_timer = tmr.create()
  poll_timer:alarm(APP.POLL_MS, tmr.ALARM_AUTO or 1, function()
    if app and app.exiting and app.exiting() then request_exit("app exiting"); return end
    update_input()
  end)
else
  log("tmr API unavailable; controller fallback polling disabled")
end

log("started", "3D 192x144 scaled + UI 320x240 SoftFP low-effects")
