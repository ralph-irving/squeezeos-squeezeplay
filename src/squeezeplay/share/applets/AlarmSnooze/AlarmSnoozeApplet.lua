local _assert, pairs = _assert, pairs
local os	       = require("os")	
local table            = require("jive.utils.table")
local string	       = require("jive.utils.string")
local debug	       = require("jive.utils.debug")
local datetime         = require("jive.utils.datetime")

local oo               = require("loop.simple")

local Applet           = require("jive.Applet")
local Framework        = require("jive.ui.Framework")
local Group            = require("jive.ui.Group")
local Icon             = require("jive.ui.Icon")
local Label            = require("jive.ui.Label")
local SimpleMenu       = require("jive.ui.SimpleMenu")
local Window           = require("jive.ui.Window")
local Popup            = require("jive.ui.Popup")
local Timer            = require("jive.ui.Timer")

local Player           = require("jive.slim.Player")
local LocalPlayer      = require("jive.slim.LocalPlayer")
local decode           = require("squeezeplay.decode")
                       
local appletManager	= appletManager
local iconbar           = iconbar
local jiveMain          = jiveMain
local jnt               = jnt

module(..., Framework.constants)
oo.class(_M, Applet)

function init(self, ...)

	self.alarmNext = self:getSettings()['alarmNext']
	self.localPlayer = Player:getLocalPlayer()
	self.server = self.localPlayer and self.localPlayer:getSlimServer()

	jnt:subscribe(self)
	self.alarmTone = "applets/AlarmSnooze/alarm.mp3"
	
	self.alarmInProgress = nil

	local timeToAlarm
	local startTimer = false
	if self.alarmNext then
		if self:_inFuture() then
			timeToAlarm = self.alarmNext
			startTimer  = true
		else
			timeToAlarm = 86400000
		end
	else
		-- arbitrarily set timeToAlarm if there isn't one, 
		-- as it will be set again whenever it is invoked by an self.alarmNext param
		timeToAlarm = 86400000
	end
	self.debugRTCTime = timeToAlarm
	self.RTCAlarmTimer = Timer(timeToAlarm,
			function ()
				log:warn("RTC ALARM FIRING")
				self:openAlarmWindow('rtc')
			end,
			true
	)
	self.wakeOnLanTimer = Timer(timeToAlarm,
			function()
				if self.server then
					log:warn('WOL packet being sent to ', self.server)
					self.server:wakeOnLan()
				end
			end,
			true
	)

	self.statusPoller = Timer(1000, 
		function ()
			local status = decode:status()
			log:warn('----------------------------------')
			log:warn('**** self.alarmInProgress:        ', self.alarmInProgress)
			log:warn('**** status.audioState:           ', status.audioState)
			log:warn('**** self.localPlayer.alarmState: ', self.localPlayer:getAlarmState())
			log:warn('**** RTC fallback running?:       ', self.RTCAlarmTimer:isRunning())
			if self.RTCAlarmTimer:isRunning() and self.debugRTCTime and self.debugRTCTime > 0 then
				local timeToAlarm = self.debugRTCTime / 1000
				log:warn('**** RTC time:       ', timeToAlarm)
				if self.debugRTCTime > 0 then	
					self.debugRTCTime = self.debugRTCTime - 1000
				end
			end
		end,
	false)

	-- this timer is for debug purposes only, to log state information every second for tracking purposes
	-- very useful when needed...
	-- self.statusPoller:start()

	if startTimer then
		self:_startTimer()
	end

	self.decodeStatePoller = Timer(10000, 
		function ()
			self:_pollDecodeState()
		end,
	false)
	
	return self
end


function notify_playerAlarmState(self, player, alarmState, alarmNext)

	log:warn('notify_playerAlarmState received for ', player, ' with alarmState of ', alarmState)
	if player:isLocal() then
		log:warn('**************************** notify_playerAlarmState received: ', alarmState, ' ', alarmNext)
		-- if there's an existing alarm window on the screen and the rtc alarm isn't firing, 
		-- we're going to hide it in the event of this notification. if alarmState is 'active', we bring up a new one
		if self.alarmInProgress ~= 'rtc' then
			self:_hideAlarmWindow()
		end

		if alarmState == 'active' then
			if player ~= Player:getCurrentPlayer() then
				log:warn('alarm has fired locally, switching to local player')
                        	appletManager:callService("setCurrentPlayer", player)
			end

			-- ignore server alarm if an alarm is already in progress
			if self.alarmInProgress == 'rtc' then
				log:warn('ignoring alarm notification because fallback fired prior')
				return
			elseif self.alarmInProgress == 'server' then
				log:info('[likely post-snooze] alarm notification received while alarm already in progress')
			    -- keep going 
			end
            
			-- stop fallback timer and proceed
			self.alarmInProgress = 'server'

			self:_stopTimer()			
			
			self:openAlarmWindow('server')

		elseif alarmState == 'snooze' then
		    
			log:warn('snooze state received')
			self.alarmInProgress = alarmState
			log:warn('self.alarmInProgress set to: ', self.alarmInProgress)
			
		elseif alarmState == 'none' then
		
			if self.alarmInProgress ~= 'rtc' then
				self.alarmNext = false
				log:info('no alarm set, clearing settings')
				self:getSettings()['alarmNext'] = false
				self:storeSettings()
				self:_setWakeupTime('none')
				self.alarmInProgress = nil
		                -- might want to qualify whether or not to stop this timer dependent upon whether it's already running.
				-- for now just log the information
				if self.RTCAlarmTimer:isRunning() then
					log:warn('clear alarm directive received while RTC timer is running!  Stopping.  Careful now...')
				end
				self:_stopDecodeStatePoller()
				self:_stopTimer()
			else
				log:warn('clear alarm directive received while fallback alarm in progress!  ignoring')
				return
			end
		elseif alarmState == 'set' then
			
			log:warn('an upcoming alarm is set, but none is currently active')
			self:_stopDecodeStatePoller()
		end
		
		-- store alarmNext data as epoch seconds
		if alarmNext and alarmNext > 0 then

			log:debug('notify_playerAlarmState: ALARMNEXT is ', alarmNext,' : NOW is ', os.time())
		
			-- want to know if this happens
			if alarmState == 'active' then
			    log:error('notify_playerAlarmState: alarmNext is ', alarmNext, '  while alarmState is ACTIVE!  ignoring...')
				return
			end
			
			self.alarmNext = alarmNext
			
			log:info('storing epochseconds of next alarm:  ', alarmNext)
		        self:getSettings()['alarmNext'] = alarmNext
			
			self:storeSettings()
			self:_setWakeupTime()
			if self.alarmInProgress ~= 'rtc' then
				self:_stopTimer()
				self:_startTimer()
			end
		end
	end
end


-- if reconnection to server occurs (which it does automatically) then local alarm file stops playing
-- or gain gets cut to almost nothing due to a lower level squeezeplay bug
-- explicitly stop local alarm audio (since no audio is being emitted anyway) and restart asynchronously
-- instantiation is asynchronous to prevent gain (or other aspect of local audio) from being modified elsewhere after we've run...
-- last requestor wins
function _alarm_sledgehammerRearm(self, caller)
	local hammer = false
	
	local status = decode:status()
	--debug.dump(status)

	log:warn('alarm_sledgehammerRearm(', caller,'): ', self.alarmInProgress, ' alarm in progress - audioState is ', status.audioState)
	if self.alarmInProgress and self.alarmInProgress ~= 'snooze' and status.audioState ~= 1 then
		hammer = true
	end

	if hammer then 
		self:_stopTimer()
		-- kickstart audio output again, asynchronously, so whatever else is messing with the audio settings is hopefully finished
		log:warn('alarm_sledgehammerRearm: audio not in good shape while alarm is firing. restart timer asynchronously at 1.5secs')
		self:_startTimer(1500)
	end
end


-- notification triggered invocation of the sledgehammer just speeds up transition to fallback alarm when said transition is required
-- (it also allows post-mortem analysis of the state transitions that have actually occurred for better evaluation of what SqueezeOS
--  is really doing behind the scenes)
-- polling would eventually manifest the transition anyway...

function notify_playerLoaded(self, player)
	log:info("notify_playerLoaded(", player, ")")
	if player == self.localPlayer then
--		self:_alarm_sledgehammerRearm('notify_playerLoaded')
		-- check for pending server alarm in case that one is pending instead, since we may have changed players to force 
		--       local control during a previous call to openAlarmWindow()
		if self.alarmInProgress == 'server' then
			log:warn("notify_playerLoaded: called while `server` alarm in progress")
		end
	end
end


function notify_playerCurrent(self, player)
	log:info("notify_playerCurrent(", player, ")")
	if player == self.localPlayer then
--		self:_alarm_sledgehammerRearm('notify_playerCurrent')
	end
end


function notify_playerModeChange(self, player, mode)
	log:warn('notify_playerModeChange: player (', player,') mode has been changed to ', mode)
	local status = decode:status()
	log:warn('notify_playerModeChange: - audioState is ', status.audioState)
end


function notify_playerConnected(self, player)
	log:warn('notify_playerConnected: ', player, ' ', self.alarmInProgress)
	if player == self.localPlayer then
--		self:_alarm_sledgehammerRearm('notify_playerConnected')
	end
end


function notify_playerDisconnected(self, player)
	log:warn('notify_playerDisconnected ', player, self.alarmInProgress)
	if player == self.localPlayer then
	end
end


-- continue playing local alarm audio if this event occurs while a local alarm is already going off.
-- just allow manual user intervention to stop playout
-- in this case, alarmInProgress will be reset when next timer is set
function notify_serverConnected(self, server)
	-- go ahead and set self.localPlayer here
	self.localPlayer = Player:getLocalPlayer()

	log:info('notify_serverConnected: ', server, ' is now connected')

	-- self.server is used for WOL purposes
	self.server = server

	if self.localPlayer then
		log:info('local player connection status is ', self.localPlayer:isConnected())
	else
		log:info('there is currently no self.localPlayer set')
	end

	-- don't want to cause error if no connection
	if self.localPlayer and self.localPlayer:isConnected() then
		log:info('                      local player->server is ', self.localPlayer:getSlimServer())
		if self.localPlayer:getSlimServer() == server then
			self:_alarm_sledgehammerRearm('notify_serverConnected')
		end
	end
end


function notify_serverDisconnected(self, server)
	log:info('notify_serverDisconnected: ', server, ' is now disconnected')

	-- blindly check state here irrespective of which server caused this notification
	if self.alarmInProgress == 'snooze' or self.alarmInProgress == 'rtc' then
		log:warn('notify_serverDisconnected: ', server, ' - disconnected, but no server alarm in progress : ', self.alarmInProgress)
	elseif self.alarmInProgress == 'server' then
		if not self.localPlayer:isConnected() then
			log:warn('notify_serverDisconnected: ', server, ' - while server alarm in progress! state ', self.alarmInProgress, ' triggering fallback alarm!')
			self:openAlarmWindow('rtc')
		else
			log:warn('notify_serverDisconnected: ', server, ' - server alarm in progress, but player still connected to ', self.localPlayer:getSlimServer())
		end
	else
		log:warn('notify_serverDisconnected: ', server, ' - disconnected, but no server alarm in progress : ', self.alarmInProgress)
	end
end


-- returns the millisecond delta between now (current time) and the epochsecs parameter
-- returns default of 1000ms if epochsecs is in the past...
function _deltaMsecs(self, epochsecs)

	local deltaSecs = epochsecs - os.time() 
	if deltaSecs <= 0 then
		log:warn('_deltaMsecs: epochsecs is in the past, deltaSecs is ', delta)
		return(1000)
	end
	-- else
	return(deltaSecs * 1000)
end


function _setWakeupTime(self, setting)
	if not setting then
		-- wakeup 3 minutes before alarm
		setting = self.alarmNext - 180
	end
	appletManager:callService("setWakeupAlarm", setting)
end


function _hideAlarmWindow(self)
	if self.alarmWindow then
		self.alarmWindow:hide()
		self.alarmWindow = nil
	end
end


function _pollDecodeState(self)
	local status = decode:status()
	if self.localPlayer:isConnected() then
		log:warn('_pollDecodeState(',self.alarmInProgress,'): audioState is ', status.audioState)
	else
		log:warn('_pollDecodeState(',self.alarmInProgress,'): audioState is ', status.audioState)
	end

	self:_alarm_sledgehammerRearm('_pollDecodeState')	
end


function soundFallbackAlarm(self)
	log:warn("soundFallbackAlarm()")
	self.localPlayer:volumeLocal(43)
	self.localPlayer:stop(true)
	self.alarmInProgress = 'rtc'
	self.localPlayer:playFileInLoop(self.alarmTone)
end

function openAlarmWindow(self, caller)

	log:warn('openAlarmWindow()', caller, ' ', self.localPlayer:isConnected())

	-- if radio is controlling a different player, switch to the local player
	-- if notify_playerLoaded needs invocation prior to player change taking effect then refire openAlarmWindow() at that time
	local currentPlayer = Player:getCurrentPlayer()

	if currentPlayer ~= self.localPlayer then
		log:warn('openAlarmWindow: switching squeezeplay control to local player (', self.localPlayer,') from (', currentPlayer,')')
		appletManager:callService("setCurrentPlayer", self.localPlayer)
		-- let notify_playerLoaded do its thing if we're not dealing with a fallback alarm
		-- originally was using notify_playerLoaded as Ben was in revision 8255, but unfortunately it also gets 
		-- called when not expected (as in when NOT switching players), so I removed to prevent multiple window problems
		-- probably need to make subsequent logic asynchronous to allow player to actually be switched/loaded, but no
		-- good options available for now since it appears notify_playerLoaded is called when least expected...
		--[[
		if not caller == 'rtc' then
			return
		end
		--]]
	end

	appletManager:callService("deactivateScreensaver")
	
	-- this method is called when the alarm time is hit
	-- when the alarm time is hit, unset the wakeup mcu time
	self:_setWakeupTime('none')

	self:_startDecodeStatePoller()

	if caller == 'server' then
		-- if we're connected, first drop the now playing window underneath the alarm window
		if self.localPlayer:isConnected() then
			appletManager:callService('goNowPlaying', Window.transitionPushLeft)
		end

		-- just informational stuff for now
		local status = decode:status()
		-- just informational
		log:warn('openAlarmWindow: called with `server` - audioState is ', status.audioState)

		if self.alarmInProgress == 'rtc' then
			log:warn('openAlarmWindow: called with `server` while `rtc` alarm in progress!')
			-- where did we come from?
			log:error('CALL STACK TRAP: ')
		end

	elseif caller == 'rtc' then
		if self.alarmInProgress ~= 'rtc' then
			log:warn('openAlarmWindow: fallback alarm activation')
		else
			log:warn('openAlarmWindow: fallback alarm snooze or explicit audio cycle')			
		end
		if not self.localPlayer then
			log:warn('openAlarmWindow: cannot play an alarm without a player')
			return
		end

		self:soundFallbackAlarm()
		
	else
		log:error('openAlarmWindow: unknown caller')
	end
	
	if self.alarmWindow then
		return
	end

	local window = Window('alarm_popup', self:string('ALARM_SNOOZE_ALARM'))

	self.time = datetime:getCurrentTime()
	local icon = Icon('icon_alarm')
	local label = Label('alarm_time', self.time)
	local headerGroup = Group('alarm_header', {
		icon = icon,
		time = label,
	})

	local menu = SimpleMenu('menu')
	menu:addItem({
		text = self:string("ALARM_SNOOZE_SNOOZE"),
		sound = "WINDOWHIDE",
		callback = function()
			self:_alarmSnooze()
			end,
	})
	menu:addItem({
		text = self:string("ALARM_SNOOZE_TURN_OFF_ALARM"),
		sound = "WINDOWHIDE",
		callback = function()
			self:_alarmOff(true)
			end,
	})	
	menu:setSelectedIndex(1)

	local cancelAction = function()
		window:playSound("WINDOWHIDE")
		self:_alarmOff(false)
		return EVENT_CONSUME
	end

	local consumeAction = function()
		log:warn('Consuming this action')
		Framework:playSound("BUMP")
		window:bumpLeft()
		return EVENT_CONSUME
	end

	local offAction = function()
		self:_alarmOff(true)
		return EVENT_UNUSED
	end
	
	local snoozeAction = function()
		self:_alarmSnooze()
		return EVENT_CONSUME
	end

	menu:addActionListener("back", self, cancelAction)
	menu:addActionListener("power", self, offAction)
	menu:addActionListener("mute", self, snoozeAction)

	window:ignoreAllInputExcept(
		--these actions are not ignored
		{ 'go', 'back', 'power', 'mute', 'volume_up', 'volume_down', 'pause' }, 
		-- consumeAction is the callback issued for all "ignored" input
		consumeAction 
	)

	menu:setHeaderWidget(headerGroup)

	window:addWidget(menu)
	window:setShowFrameworkWidgets(false)
	window:setAllowScreensaver(false)
	window:show(Window.transitionFadeIn)

	window:addTimer(1000, 
			function() 
				self:_updateTime() 
			end
	)

	window:setAlwaysOnTop(true)
	self.alarmWindow = window
	self.timeWidget  = label

end


function _updateTime(self) 	 
	local time = datetime:getCurrentTime() 	 
	if time ~= self.time then 	 
		log:debug('updating time in alarm window') 	 
		self.time = time 	 
		self.timeWidget:setValue(time) 	 
	end 	 
end


function _alarmOff(self, stopStream)
	if self.alarmInProgress == 'rtc' then
		self.localPlayer:stop(true)
		iconbar:setAlarm('OFF')
		log:warn('_alarmOff: RTC alarm canceled')
	else
		if self.localPlayer:isConnected() then
			log:warn('_alarmOff: server alarm canceled - alarmInProgress state (', self.alarmInProgress, ')')
		else
			log:warn('_alarmOff: player not connected! - alarmInProgress state (', self.alarmInProgress, ')')
		end
	end
	
	self.alarmInProgress = nil
	self:_stopTimer()
	self.alarmWindow:playSound("WINDOWHIDE")
	self:_hideAlarmWindow()
	
	self:_stopDecodeStatePoller()

	if self.localPlayer:isConnected() and stopStream then
		log:warn('_alarmOff: send stopAlarm to connected server')
		self.localPlayer:stopAlarm()
	end
end


function _stopTimer(self)
	if self.RTCAlarmTimer:isRunning() then
		log:warn('_stopTimer: stopping RTC fallback alarm timer')
		self.RTCAlarmTimer:stop()
		self.debugRTCTime = 0
	end
	if self.wakeOnLanTimer:isRunning() then
		log:warn('_stopTimer: stopping WOL timer')
		self.wakeOnLanTimer:stop()
	end
end


function _stopDecodeStatePoller(self)
	if self.decodeStatePoller:isRunning() then
		log:warn('stopping decodeStatePoller')
		self.decodeStatePoller:stop()
	end
end


function _startDecodeStatePoller(self)
	if self.decodeStatePoller:isRunning() then
		self.decodeStatePoller:restart()
	else
		self.decodeStatePoller:start()
	end
end


function _inFuture(self)
	if not self.alarmNext then
		return false
	end

	local now = os.time()
	if self.alarmNext - now > 0 then
		return true
	end
	return false
end


function _startTimer(self, interval)
	
	if not self.alarmNext and not interval then
		log:error('both alarmNext and interval have no value!')	
		return
	end

	if self.RTCAlarmTimer:isRunning() then
		self.RTCAlarmTimer:stop()
		self.debugRTCTime = 0
		log:warn('_startTimer: stopping RTC fallback alarm timer')
	end
	
	if interval then
		log:warn('starting RTC fallback alarm timer for interval ', interval)
		self.RTCAlarmTimer:setInterval(interval)
		self.debugRTCTime = interval
	else
		-- get msecs between now and requested alarm
		-- add 10 secs for fallback timer to bias alarm toward server wakeup
		local sleepMsecs = self:_deltaMsecs(self.alarmNext + 10);
		log:warn('_startTimer: starting RTC fallback alarm timer (', sleepMsecs, ')')
		self.RTCAlarmTimer:setInterval(sleepMsecs)
		self.debugRTCTime = sleepMsecs

		-- WOL timer is set when sleepMsecs is more than 11 minutes away (660,000 msecs)
		if sleepMsecs > 660000 then
			self.wakeOnLanTimer:setInterval(sleepMsecs - 600000)
			if self.wakeOnLanTimer:isRunning() then
				self.wakeOnLanTimer:restart()
			else
				self.wakeOnLanTimer:start()
			end
		end
	end
	self.RTCAlarmTimer:start()
end


function _alarmSnooze(self)
	
	log:warn('_alarmSnooze: alarmInProgress is ', self.alarmInProgress, ' : connection status is ', self.localPlayer:isConnected())

	self:_stopTimer()

	local alarmSnoozeSeconds = self.localPlayer:getAlarmSnoozeSeconds()
	log:warn('_alarmSnooze: fallback alarm snoozing for ', alarmSnoozeSeconds,'  + 20 seconds')
	self:_startTimer(alarmSnoozeSeconds * 1000 + 20000 )
	
	if self.alarmInProgress == 'rtc' then
		log:warn('_alarmSnooze: stopping fallback alarm audio')
		-- stop playback
		self.localPlayer:stop(true)
	else
		self.alarmInProgress = 'snooze'
	end

	if self.localPlayer:isConnected() then
		log:warn('_alarmSnooze: sending snooze command to connected server for connected player ', self.localPlayer)
		self.localPlayer:snooze()
	end
	self:_stopDecodeStatePoller()

	self.alarmWindow:playSound("WINDOWHIDE")
	self:_hideAlarmWindow()
end


function free(self)
	self.alarmWindow = nil
	return false
end

--[[

=head1 LICENSE

Copyright 2010 Logitech. All Rights Reserved.

This file is licensed under BSD. Please see the LICENSE file for details.

=cut
--]]
