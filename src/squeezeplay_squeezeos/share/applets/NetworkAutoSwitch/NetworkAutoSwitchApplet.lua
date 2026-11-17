
--[[
=head1 NAME

applets.NetworkAutoSwitch.NetworkAutoSwitchApplet

=head1 DESCRIPTION

Switches automatically between the wired and the wireless network:
when an Ethernet cable is plugged in the player moves to the wired
network (and wifi is released), when the cable is unplugged it goes
back to the wifi network it used before.

The wired link is kept administratively up while wifi is in use, so
the cable can be detected; it has no IP address in that state. Cable
detection is paused while running on battery.

=cut
--]]

local pcall, tonumber, tostring = pcall, tonumber, tostring


local oo                     = require("loop.simple")
local io                     = require("io")
local os                     = require("os")
local string                 = require("string")

local Applet                 = require("jive.Applet")
local Checkbox               = require("jive.ui.Checkbox")
local Framework              = require("jive.ui.Framework")
local SimpleMenu             = require("jive.ui.SimpleMenu")
local Task                   = require("jive.ui.Task")
local Textarea               = require("jive.ui.Textarea")
local Timer                  = require("jive.ui.Timer")
local Window                 = require("jive.ui.Window")
local Networking             = require("jive.net.Networking")

local jnt                    = jnt

module(..., Framework.constants)
oo.class(_M, Applet)


local POLL_INTERVAL   = 3000  -- ms between cable checks
local DEBOUNCE_POLLS  = 2     -- equal readings needed before switching
local DHCP_TIMEOUT    = 30    -- seconds to wait for a wired address
local WIFI_TIMEOUT    = 60    -- seconds to wait for wifi before giving up on the notify
local CHARGER_STATE   = "/sys/devices/platform/i2c-adapter:i2c-1/1-0010/charger_state"


local function _readFile(path)
	local f = io.open(path, "r")
	if not f then
		return nil
	end
	local value = f:read("*l")
	f:close()
	return value
end


function init(self)
	self.eth  = Networking:wiredInterface(jnt)
	self.wlan = Networking:wirelessInterface(jnt)

	self.busy = false
	self.pending = nil       -- "wired" or "wifi" while debouncing
	self.pendingCount = 0
	self.wiredFailed = false -- DHCP failed on this cable; wait for replug

	if self:getSettings().enabled then
		self:_start()
	end
end


-- resident: the watcher keeps running when the settings window closes
function free(self)
	return false
end


function _supported(self)
	return self.eth ~= nil and self.wlan ~= nil
end


function _start(self)
	if not self:_supported() or self.timer then
		return
	end
	log:info("cable/wifi auto switch started")
	self.timer = Timer(POLL_INTERVAL, function() self:_poll() end)
	self.timer:start()
	self:_poll()
end


function _stop(self)
	if self.timer then
		self.timer:stop()
		self.timer = nil
	end
	self.pending = nil
	self.pendingCount = 0
	log:info("cable/wifi auto switch stopped")

	-- leave the wired link as the stock firmware does: down unless in use
	if self:_supported() and not self:_wiredActive() then
		os.execute("/bin/ip link set " .. self.eth:getName() .. " down")
	end
end


function _onBattery(self)
	local state = tonumber(_readFile(CHARGER_STATE) or "")
	if not state then
		return false   -- no battery hardware (e.g. Touch): mains powered
	end
	-- bit 3 = charging (so on mains); bits 0+1 = running on battery
	return (state & 8) == 0 and (state & 3) == 3
end


function _wiredActive(self)
	local ok, active = pcall(self.eth._ifstate, self.eth)
	return ok and active and true or false
end


function _wifiActive(self)
	local ok, active = pcall(self.wlan._ifstate, self.wlan)
	if ok and active and active ~= true and active ~= "" then
		return active
	end
	return false
end


-- returns true/false, or nil when the link is down and can't tell
function _carrier(self)
	local name = self.eth:getName()
	local flags = tonumber(_readFile("/sys/class/net/" .. name .. "/flags") or "")
	if not flags or (flags % 2) == 0 then
		-- interface is administratively down: bring the link up (no address)
		-- so the PHY can report whether a cable is plugged in
		os.execute("/bin/ip link set " .. name .. " up")
		return nil
	end
	local carrier = _readFile("/sys/class/net/" .. name .. "/carrier")
	if carrier == nil then
		return nil
	end
	return carrier == "1"
end


-- usable IPv4 address of an interface; self-assigned 169.254.x.x doesn't count
function _address(self, iface)
	local f = io.popen("/bin/ip -4 addr show " .. iface:getName() .. " 2>/dev/null")
	if not f then
		return nil
	end
	local out = f:read("*a")
	f:close()
	for ip in string.gmatch(out, "inet (%d+%.%d+%.%d+%.%d+)") do
		if not string.match(ip, "^169%.254%.") then
			return ip
		end
	end
	return nil
end


-- after switching to wifi: tell the server connections once wifi is usable
function _checkWifiReady(self)
	if not self.wifiSince then
		return
	end
	local ip = self:_address(self.wlan)
	if ip then
		log:info("wifi network up, address ", ip)
		self.wifiSince = nil
		jnt:notify("networkConnected")
	elseif os.time() - self.wifiSince > WIFI_TIMEOUT then
		log:warn("wifi did not come up within ", WIFI_TIMEOUT, " seconds")
		self.wifiSince = nil
	end
end


function _poll(self)
	if self.busy then
		return
	end

	self:_checkWifiReady()

	local wiredActive = self:_wiredActive()

	if not wiredActive and not self.wiredSince and self:_onBattery() then
		-- don't power the Ethernet PHY on battery
		self.pending = nil
		local name = self.eth:getName()
		local flags = tonumber(_readFile("/sys/class/net/" .. name .. "/flags") or "")
		if flags and (flags % 2) == 1 then
			os.execute("/bin/ip link set " .. name .. " down")
		end
		return
	end

	local carrier = self:_carrier()
	if carrier == nil then
		return
	end

	if not carrier then
		-- replugging the cable gives wired another chance
		self.wiredFailed = false
	end

	-- a wired attempt is pending: wait for DHCP, give up after DHCP_TIMEOUT.
	-- This also covers a failed ifup (no ifstate entry): wifi is already
	-- down then, so don't retry wired but fall back once the deadline passes.
	if self.wiredSince then
		if not carrier then
			log:info("cable unplugged during wired attempt, going back to wifi")
			self:_switch(_toWifiTask)
			return
		end
		local ip = wiredActive and self:_address(self.eth)
		if ip then
			log:info("wired network up, address ", ip)
			self.wiredSince = nil
			jnt:notify("networkConnected")
		elseif os.time() - self.wiredSince > DHCP_TIMEOUT then
			log:warn("no address on the wired network, going back to wifi")
			self.wiredFailed = true
			self:_switch(_toWifiTask)
		end
		return
	end

	local want = nil
	if carrier and not wiredActive and not self.wiredFailed then
		want = "wired"
	elseif not carrier and wiredActive then
		want = "wifi"
	end

	if want ~= self.pending then
		self.pending = want
		self.pendingCount = 0
	end
	if not want then
		return
	end

	self.pendingCount = self.pendingCount + 1
	if self.pendingCount < DEBOUNCE_POLLS then
		return
	end

	self.pending = nil
	self.pendingCount = 0

	if want == "wired" then
		self:_switch(_toWiredTask)
	else
		self:_switch(_toWifiTask)
	end
end


function _switch(self, taskFunc)
	self.busy = true
	Task("networkAutoSwitch", self, function()
		-- Task:pcall, because the network calls yield inside the task
		local ok, err = Task:pcall(taskFunc, self)
		if not ok then
			log:error("network switch failed: ", err)
		end
		self.busy = false
	end):addTask()
end


-- make sure /etc/network/interfaces has a block for the wired interface
function _ensureWiredConfig(self)
	local name = self.eth:getName()
	local f = io.open("/etc/network/interfaces", "r")
	if f then
		for line in f:lines() do
			if string.match(line, "^iface%s+" .. name .. "%s") then
				f:close()
				return
			end
		end
		f:close()
	end
	log:info("adding dhcp config for ", name)
	self.eth:_editNetworkInterfaces(name, "dhcp", "script /etc/network/udhcpc_action")
end


function _toWiredTask(self)
	local ssid = self:_wifiActive()
	if ssid then
		self:getSettings().lastSsid = ssid
		self:storeSettings()
	end

	log:info("cable plugged in, switching to wired (leaving wifi ", tostring(ssid), ")")

	-- _poll checks that DHCP gave us an address; set before switching so a
	-- failure inside t_selectNetwork still ends in the wifi fallback
	self.wiredSince = os.time()
	self.wifiSince = nil

	self:_ensureWiredConfig()
	-- brings wifi down and the wired interface up (DHCP)
	self.eth:t_selectNetwork(self.eth:getName())
end


-- the wifi network to return to: the last one used, else the first configured
function _wifiSsid(self)
	local ssid = self:getSettings().lastSsid
	if ssid then
		return ssid
	end

	local list = self.wlan:request("LIST_NETWORKS")
	for id, nssid in string.gmatch(list, "\n(%d+)\t([^\t]*)") do
		return nssid
	end
	return nil
end


function _toWifiTask(self)
	self.wiredSince = nil
	local ssid = self:_wifiSsid()
	if not ssid then
		log:warn("cable unplugged, but no wifi network is configured")
		return
	end

	log:info("switching to wifi ", ssid)
	self.wlan:t_selectNetwork(ssid)
	-- association and DHCP finish in the background; _poll notifies
	self.wifiSince = os.time()
end


function settingsShow(self, menuItem)
	local window = Window("text_list", menuItem.text, 'settingstitle')
	local menu = SimpleMenu("menu")

	if not self:_supported() then
		menu:setHeaderWidget(Textarea("help_text", self:string("AUTOSWITCH_UNSUPPORTED")))
		window:addWidget(menu)
		window:show()
		return window
	end

	menu:setHeaderWidget(Textarea("help_text", self:string("AUTOSWITCH_HELP")))

	menu:addItem({
		text  = self:string("AUTOSWITCH_ENABLE"),
		style = 'item_choice',
		check = Checkbox("checkbox",
			function(_, isSelected)
				local settings = self:getSettings()
				settings.enabled = isSelected
				self:storeSettings()
				if isSelected then
					self:_start()
				else
					self:_stop()
				end
			end,
			self:getSettings().enabled
		),
	})

	window:addWidget(menu)
	-- not tieAndShowWindow: this applet stays resident
	window:show()
	return window
end


--[[

=head1 LICENSE

This file is licensed under BSD. Please see the LICENSE file for details.

=cut
--]]
