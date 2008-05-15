
-- Private class to handle player position scanner 

local tostring, tonumber = tostring, tonumber

local oo                     = require("loop.base")
local os                     = require("os")
local math                   = require("math")
local string	             = require("string")

local Framework              = require("jive.ui.Framework")
local Group                  = require("jive.ui.Group")
local Icon                   = require("jive.ui.Icon")
local Label                  = require("jive.ui.Label")
local Popup                  = require("jive.ui.Popup")
local Slider                 = require("jive.ui.Slider")
local Timer                  = require("jive.ui.Timer")
local Window                 = require("jive.ui.Window")

local debug                  = require("jive.utils.debug")
local log                    = require("jive.utils.log").logger("player")


local EVENT_KEY_ALL          = jive.ui.EVENT_KEY_ALL
local EVENT_KEY_PRESS        = jive.ui.EVENT_KEY_PRESS
local EVENT_KEY_HOLD         = jive.ui.EVENT_KEY_HOLD
local EVENT_KEY_DOWN         = jive.ui.EVENT_KEY_DOWN
local EVENT_KEY_UP           = jive.ui.EVENT_KEY_UP
local EVENT_SCROLL           = jive.ui.EVENT_SCROLL

local EVENT_CONSUME          = jive.ui.EVENT_CONSUME
local EVENT_UNUSED           = jive.ui.EVENT_UNUSED

local KEY_GO                 = jive.ui.KEY_GO
local KEY_BACK               = jive.ui.KEY_BACK
local KEY_FWD                = jive.ui.KEY_FWD
local KEY_REW                = jive.ui.KEY_REW

-- Tuning
local POSITION_STEP = 5
local POPUP_AUTOCLOSE_INTERVAL = 10000  -- close popup after this much inactivity
local AUTOINVOKE_INTERVAL_LOCAL = 400	-- invoke gotoTime after this much inactivity for local tracks
local AUTOINVOKE_INTERVAL_REMOTE = 2000	-- and this much for remote streams
local ACCELERATION_INTERVAL = 350       -- events faster than this cause acceleration
local ACCELERATION_INTERVAL_SLOW = 200  -- but less so unless faster than this


module(..., oo.class)

local function _secondsToString(seconds)
	local min = math.floor(seconds / 60)
	local sec = math.floor(seconds - (min*60))

	return string.format("%d:%02d", min, sec)
end

local function _updateDisplay(self)
	self.title:setValue(self.applet:string("SLIMBROWSER_SCANNER"))
	self.slider:setValue(tonumber(self.elapsed))
	local strElapsed = _secondsToString(self.elapsed)
	local strRemain = "-" .. _secondsToString(self.duration - self.elapsed)

	self.scannerGroup:setWidgetValue("elapsed", strElapsed)
	self.scannerGroup:setWidgetValue("remain", strRemain)
end


local function _updateElapsedTime(self)
	if not self.popup then
		self.displayTimer:stop()
		self.holdTimer:stop()
		return
	end

	self.elapsed, self.duration = self.player:getTrackElapsed()
	_updateDisplay(self)
end


local function _openPopup(self)
	if self.popup or not self.player then
		return
	end

	-- we need a local copy of the elapsed time
	self.elapsed, self.duration = self.player:getTrackElapsed()
	if not self.elapsed or not self.duration or not self.player:isTrackSeekable() then
		-- don't show the popup if the player state is not loaded
		-- or if we cannot seek in this track
		return
	end
	
	local popup = Popup("scannerPopup")
	popup:setAutoHide(false)

	local title = Label("title", "")
	popup:addWidget(title)

	local slider = Slider("scanner")
	slider:setRange(0, tonumber(self.duration), tonumber(self.elapsed))
	self.scannerGroup = Group("scannerGroup", {
					      elapsed = Label("text", ""),
					      slider = slider,
					      remain = Label("text", "")
				      })
	popup:addWidget(self.scannerGroup)
	popup:addListener(EVENT_KEY_ALL | EVENT_SCROLL,
			  function(event)
				  return self:event(event)
			  end)

	-- we handle events
	popup.brieflyHandler = false

	-- open the popup
	self.popup = popup
	self.title = title
	self.slider = slider

	self.displayTimer:restart()

	if self.player:isRemote() then
		self.autoinvokeTime = AUTOINVOKE_INTERVAL_REMOTE
	else
		self.autoinvokeTime = AUTOINVOKE_INTERVAL_LOCAL	
	end

	_updateDisplay(self)

	popup:showBriefly(POPUP_AUTOCLOSE_INTERVAL,
		function()
			self.popup = nil
		end,
		Window.transitionPushPopupUp,
		Window.transitionPushPopupDown
	)

end


local function _updateSelectedTime(self)
	if not self.popup then
		self.displayTimer:stop()
		self.holdTimer:stop()
		return
	end
	if self.delta == 0 then
		return
	end

	-- Now that the user has changed the position, stop tracking the actual playing position
	self.displayTimer:stop()

	-- keep the popup window open
	self.popup:showBriefly()

	-- accelation
	local now = Framework:getTicks()
	local interval = now - self.lastUpdate
	if self.accelDelta ~= self.delta or interval > ACCELERATION_INTERVAL then
		self.accelCount = 0
	end

	self.accelCount = math.min(self.accelCount + 1, self.duration/15, 50)
	self.accelDelta = self.delta
	self.lastUpdate = now

	-- change position
	local accel
	if interval > ACCELERATION_INTERVAL_SLOW then
		accel = self.accelCount / 15
	else
		accel = self.accelCount / 10
	end
	local new = math.abs(self.elapsed) + self.delta * accel * POSITION_STEP
	
	if new > self.duration then 
		new = self.duration
	elseif new < 0 then
		new = 0
	end

	-- self.elapsed = self.player:gotoTime(new) or self.elapsed
	self.elapsed = new
	_updateDisplay(self)
	
	self.autoInvokeTimer:restart(self.autoinvokeTime)
end


function _gotoTime(self)
	self.autoInvokeTimer:stop()
	if not self.popup then
		return
	end
	self.player:gototime(math.floor(self.elapsed))
	self.displayTimer:restart()
end

function __init(self, applet)
	local obj = oo.rawnew(self, {})

	obj.applet = applet
	obj.lastUpdate = 0
	obj.displayTimer = Timer(1000, function() _updateElapsedTime(obj) end)
	obj.autoInvokeTimer = Timer(AUTOINVOKE_INTERVAL_LOCAL, function() _gotoTime(obj) end, true)
	obj.holdTimer = Timer(100, function() _updateSelectedTime(obj) end)

	return obj
end


function setPlayer(self, player)
	self.player = player
end


function event(self, event)
	local onscreen = true
	if not self.popup then
		onscreen = false
		_openPopup(self)
	end

	local type = event:getType()
	
	if type == EVENT_SCROLL then
		local scroll = event:getScroll()

		if scroll > 0 then
			self.delta = 1
		elseif scroll < 0 then
			self.delta = -1
		else
			self.delta = 0
		end
		_updateSelectedTime(self)

	elseif type == EVENT_KEY_PRESS then
		local keycode = event:getKeycode()

		-- GO closes the popup & executes any pending change
		if keycode & KEY_GO ~= 0 then
			if self.autoInvokeTimer:isRunning() then _gotoTime(self) end
			self.popup:showBriefly(0)
			return EVENT_CONSUME
		-- BACK closes the popup & cancels any pending change
		elseif keycode & KEY_BACK ~= 0 then
			self.autoInvokeTimer:stop()
			self.popup:showBriefly(0)
                        return EVENT_CONSUME
		end

		-- any other keys forward to the lower window
		if keycode & (KEY_FWD|KEY_REW) == 0 then
			local lower = self.popup:getLowerWindow()
			if lower then
				Framework:dispatchEvent(lower, event)
			end

			self.popup:showBriefly(0)
			return EVENT_CONSUME
		end

		return EVENT_CONSUME
	else
		local keycode = event:getKeycode()

		-- we're only interested in volume keys
		if keycode & (KEY_FWD|KEY_REW) == 0 then
			return EVENT_CONSUME
		end

		-- stop volume update on key up
		if type == EVENT_KEY_UP then
			self.delta = 0
			self.muting = false
			self.holdTimer:stop()
			return EVENT_CONSUME
		end

		-- update position
		-- We could add "or type == EVENT_KEY_HOLD" to this test,
		-- in which case the hold-fwd/hold-rew used to enter this mode
		-- would immediately start scanning, but I think that it is better
		-- without this.
		if type == EVENT_KEY_DOWN then
			if keycode == KEY_FWD then
				self.delta = 1
			elseif keycode == KEY_REW then
				self.delta = -1
			else
				self.delta = 0
			end

			self.holdTimer:restart()
			if onscreen then
				_updateSelectedTime(self)
			end

			return EVENT_CONSUME
		end
	end

	return EVENT_CONSUME
end


--[[

=head1 LICENSE

Copyright 2007 Logitech. All Rights Reserved.

This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.

=cut
--]]