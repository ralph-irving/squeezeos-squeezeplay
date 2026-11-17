
local oo            = require("loop.simple")

local AppletMeta    = require("jive.AppletMeta")

local appletManager = appletManager
local jiveMain      = jiveMain


module(...)
oo.class(_M, AppletMeta)


function jiveVersion(self)
	return 1, 1
end


function defaultSettings(self)
	return {
		enabled = false,
		-- wifi network to go back to when the cable is unplugged
		lastSsid = false,
	}
end


function registerApplet(self)
	jiveMain:addItem(self:menuItem('appletNetworkAutoSwitch', 'networkSettings', "AUTOSWITCH_TITLE", function(applet, ...) applet:settingsShow(...) end, 105))
end


function configureApplet(self)
	if self:getSettings().enabled then
		-- the watcher must keep running in the background
		appletManager:loadApplet("NetworkAutoSwitch")
	end
end


--[[

=head1 LICENSE

This file is licensed under BSD. Please see the LICENSE file for details.

=cut
--]]
