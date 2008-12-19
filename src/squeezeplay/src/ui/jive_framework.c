/*
** Copyright 2007 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/


#include "common.h"
#include "jive.h"

#include <time.h>

#include <SDL_syswm.h>

int (*jive_sdlevent_pump)(lua_State *L);
int (*jive_sdlfilter_pump)(const SDL_Event *event);

SDL_Rect jive_dirty_region;

/* global counter used to invalidate widget skin and layout */
Uint32 jive_origin = 0;
static Uint32 next_jive_origin = 0;


/* performance warning thresholds, 0 = disabled */
struct jive_perfwarn perfwarn = { 0, 0, 0, 0, 0, 0 };

SDLPango_Context *pangocontext;

/* Frame rate calculations */
//static Uint32 framedue = 0;
//static Uint32 framerate = 1000 / JIVE_FRAME_RATE;


/* button hold threshold 1 seconds */
#define HOLD_TIMEOUT 1000

#define POINTER_TIMEOUT 20000

static bool update_screen = true;

static JiveTile *jive_background = NULL;

static Uint16 screen_w, screen_h, screen_bpp;

static bool screen_isfull = false;

struct jive_keymap {
	SDLKey keysym;
	JiveKey keycode;
};

static enum jive_key_state {
	KEY_STATE_NONE,
	KEY_STATE_DOWN,
	KEY_STATE_SENT,
} key_state = KEY_STATE_NONE;

static enum jive_mouse_state {
	MOUSE_STATE_NONE,
	MOUSE_STATE_DOWN,
	MOUSE_STATE_DRAG,
	MOUSE_STATE_SENT,
} mouse_state = MOUSE_STATE_NONE;

static JiveKey key_mask = 0;

static SDL_TimerID key_timer = NULL;

static SDL_TimerID mouse_timer = NULL;

static SDL_TimerID pointer_timer = NULL;

static Uint16 mouse_origin_x, mouse_origin_y;

static struct jive_keymap keymap[] = {
	{ SDLK_RIGHT,		JIVE_KEY_GO },
	{ SDLK_RETURN,		JIVE_KEY_GO },
	{ SDLK_LEFT,		JIVE_KEY_BACK },
	{ SDLK_HOME,		JIVE_KEY_HOME },
	{ SDLK_AudioPlay,	JIVE_KEY_PLAY },
	{ SDLK_AudioPause,	JIVE_KEY_PAUSE },
	{ SDLK_KP_PLUS,		JIVE_KEY_ADD },
	{ SDLK_AudioPrev,	JIVE_KEY_REW },
	{ SDLK_AudioNext,	JIVE_KEY_FWD },
	{ SDLK_AudioRaiseVolume,JIVE_KEY_VOLUME_UP },
	{ SDLK_AudioLowerVolume,JIVE_KEY_VOLUME_DOWN },
	{ SDLK_PAGEUP,      JIVE_KEY_PAGE_UP },
	{ SDLK_PAGEDOWN,    JIVE_KEY_PAGE_DOWN },
	{ SDLK_UNKNOWN,		JIVE_KEY_NONE },
};


static int process_event(lua_State *L, SDL_Event *event);
static int filter_events(const SDL_Event *event);
int jiveL_update_screen(lua_State *L);


int jive_traceback (lua_State *L) {
	lua_getfield(L, LUA_GLOBALSINDEX, "debug");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return 1;
	}
	lua_pushvalue(L, 1);  /* pass error message */
	lua_pushinteger(L, 2);  /* skip this function and traceback */
	lua_call(L, 2, 1);  /* call debug.traceback */
	return 1;
}


static int jiveL_init(lua_State *L) {
	SDL_Rect r;
	JiveSurface *srf, *splash, *icon;
	Uint16 splash_w, splash_h;

	/* screen properties */
	lua_getfield(L, 1, "screen");
	if (lua_isnil(L, -1)) {
		luaL_error(L, "Framework.screen is ni");
	}

	lua_getfield(L, -1, "bounds");
	jive_torect(L, -1, &r);
	lua_pop(L, 1);

	lua_getfield(L, -1, "bpp");
	screen_bpp = luaL_optint(L, -1, 16);
	lua_pop(L, 1);

	screen_w = r.w;
	screen_h = r.h;

	/* linux fbcon does not need a mouse */
	SDL_putenv("SDL_NOMOUSE=1");

	/* initialise SDL */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "SDL_Init(V|T|A): %s\n", SDL_GetError());
		SDL_Quit();
		exit(-1);
	}
	if (SDLPango_Init() == -1) {
		fprintf(stderr, "SDLPango Init failed.\n");
		SDL_Quit();
		exit(-1);
	}

	pangocontext = SDLPango_CreateContext_GivenFontDesc("FreeSans Bold 15");
    
    SDLPango_SetDpi(pangocontext, 75.0, 75.0);
	
	SDLPango_SetDefaultColor(pangocontext, MATRIX_TRANSPARENT_BACK_WHITE_LETTER);
	SDLPango_SetMinimumSize(pangocontext, 0, 0);
	
	/* Register callback for additional events (used for multimedia keys)*/
	SDL_EventState(SDL_SYSWMEVENT,SDL_ENABLE);
	SDL_SetEventFilter(filter_events);

	platform_init(L);

	/* open window */
	SDL_WM_SetCaption("SqueezePlay Beta", "SqueezePlay Beta");

	srf = jive_surface_set_video_mode(screen_w, screen_h, screen_bpp, false);
	if (!srf) {
		SDL_Quit();
		exit(-1);
	}

	/* load the icon */
	icon = jive_surface_load_image("jive/app.png");
	jive_surface_set_wm_icon(icon);
	jive_surface_free(icon);

	SDL_ShowCursor(SDL_DISABLE);
	SDL_EnableKeyRepeat (100, 100);
	SDL_EnableUNICODE(1);

	tolua_pushusertype(L, srf, "Surface");
	lua_setfield(L, -2, "surface");

	/* background image */
	jive_background = jive_tile_fill_color(0x000000FF);

	/* show splash screen */
	splash = jive_surface_load_image("jive/splash.png");
	if (splash) {
		jive_surface_get_size(splash, &splash_w, &splash_h);
		jive_surface_blit(splash, srf, (screen_w - splash_w) / 2, (screen_h - splash_h) / 2);
		jive_surface_flip(srf);
	}

	/* jive.ui.style = {} */
	lua_getglobal(L, "jive");
	lua_getfield(L, -1, "ui");
	lua_newtable(L);
	lua_setfield(L, -2, "style");
	lua_pop(L, 2);

	return 0;
}

static int filter_events(const SDL_Event *event)
{
	if (jive_sdlfilter_pump) {
		return jive_sdlfilter_pump(event);
	}
	
	return 1;
}

void jive_send_key_event(JiveEventType keyType, JiveKey keyCode) {
	JiveEvent keyEvent;
	memset(&keyEvent, 0, sizeof(JiveEvent));
	
	keyEvent.type = keyType;
	keyEvent.ticks = SDL_GetTicks();
	keyEvent.u.key.code = keyCode;
	jive_queue_event(&keyEvent);
}

static int jiveL_quit(lua_State *L) {

	/* de-reference all windows */
	jiveL_getframework(L);
	lua_pushnil(L);
	lua_setfield(L, -2, "windowStack");
	lua_pop(L, 1);

	/* force lua GC */
	lua_gc(L, LUA_GCCOLLECT, 0);

	/* quit SDL */
	SDL_Quit();

	return 0;
}


static int jiveL_process_events(lua_State *L) {
	Uint32 r = 0;
	SDL_Event event;

	/* stack:
	 * 1 : jive.ui.Framework
	 */

	JIVEL_STACK_CHECK_BEGIN(L);

	/* Exit if we have no windows */
	lua_getfield(L, 1, "windowStack");
	if (lua_objlen(L, -1) == 0) {
		lua_pop(L, 1);

		lua_pushboolean(L, 0);
		return 1;
	}
	lua_rawgeti(L, -1, 1);


	/* pump keyboard/mouse events once per frame */
	SDL_PumpEvents();

	if (jive_sdlevent_pump) {
		jive_sdlevent_pump(L);
	}

	/* check queue size */
	if (perfwarn.queue) {
		if (SDL_EventQueueLength() > perfwarn.queue) {
			printf("SDL_event_queue > %2d : %3d\n", perfwarn.queue, SDL_EventQueueLength());
		}
	}

	/* process events */
	while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_ALLEVENTS) > 0 ) {
		r |= process_event(L, &event);
	}

	lua_pop(L, 2);
	
	JIVEL_STACK_CHECK_END(L);

	if (r & JIVE_EVENT_QUIT) {
		lua_pushboolean(L, 0);
		return 1;
	}

	lua_pushboolean(L, 1);
	return lua_yield(L, 1);
}


int jiveL_set_update_screen(lua_State *L) {
	/* stack is:
	 * 1: framework
	 * 2: enable/disable screen updates
	 */

	bool old_update_screen = update_screen;
	update_screen = lua_toboolean(L, 2);

	if (update_screen && !old_update_screen) {
		/* cancel any pending transitions */
		lua_pushnil(L);
		lua_setfield(L, 1, "transition");

		/* redraw now */
		lua_pushcfunction(L, jiveL_update_screen);
		lua_pushvalue(L, 1);
		lua_call(L, 1, 0);

		/* short delay to allow video buffer to flip */
		SDL_Delay(50);
	}

	return 0;
}


static int _update_screen(lua_State *L) {
	JiveSurface *srf;
	Uint32 t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
	clock_t c0 = 0, c1 = 0;

	JIVEL_STACK_CHECK_BEGIN(L);

	if (!update_screen) {
		return 0;
	}

	/* stack is:
	 * 1: framework
	 */

	lua_getfield(L, 1, "screen");
	lua_getfield(L, -1, "surface");
	srf = tolua_tousertype(L, -1, 0);
	lua_replace(L, -2);


	/* Exit if we have no windows. We need to check
	 * again as the event handlers may have changed
	 * the window stack */
	lua_getfield(L, 1, "windowStack");
	if (lua_objlen(L, -1) == 0) {
		lua_pop(L, 2);

		JIVEL_STACK_CHECK_ASSERT(L);
		return 0;
	}
	lua_rawgeti(L, -1, 1);


	if (perfwarn.screen) {
		t0 = SDL_GetTicks();
		c0 = clock();
	}


	do {
		jive_origin = next_jive_origin;

		/* Layout window and widgets */
		if (jive_getmethod(L, -1, "checkLayout")) {
			lua_pushvalue(L, -2);
			lua_call(L, 1, 0);
		}

		/* check in case the origin changes during layout */
	} while (jive_origin != next_jive_origin);

	if (perfwarn.screen) t1 = SDL_GetTicks();
 
	/* Widget animations */
	lua_getfield(L, 1, "animations");
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		lua_getfield(L, -1, "animations");
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			int frame;

			/* stack is:
			 * -2: key
			 * -1: table
			 */
			lua_rawgeti(L, -1, 2);
			frame = lua_tointeger(L, -1) - 1;

			if (frame == 0) {
				lua_rawgeti(L, -2, 1); // function
				lua_pushvalue(L, -6); // widget
				lua_call(L, 1, 0);
				// function is poped by lua_call
				
				lua_rawgeti(L, -2, 3);
				lua_rawseti(L, -3, 2);
			}
			else {
				lua_pushinteger(L, frame);
				lua_rawseti(L, -3, 2);
			}
			lua_pop(L, 2);
		}
		lua_pop(L, 2);
	}
	lua_pop(L, 1);

	if (perfwarn.screen) t2 = SDL_GetTicks();

	/* Window transitions */
	lua_getfield(L, 1, "transition");
	if (!lua_isnil(L, -1)) {
		/* Draw background */
		jive_surface_set_clip(srf, NULL);
		jive_tile_set_alpha(jive_background, 0); // no alpha channel
		jive_tile_blit(jive_background, srf, 0, 0, screen_w, screen_h);

		if (perfwarn.screen) t3 = SDL_GetTicks();
		
		/* Animate screen transition */
		lua_pushvalue(L, -1);
		lua_pushvalue(L, -3);  	// widget
		lua_pushvalue(L, 2);	// surface
		lua_call(L, 2, 0);
		
		if (perfwarn.screen) t4 = SDL_GetTicks();
		jive_surface_flip(srf);
	}
	else if (jive_dirty_region.w) {
#if 0
		printf("REDRAW: %d,%d %dx%d\n", jive_dirty_region.x, jive_dirty_region.y, jive_dirty_region.w, jive_dirty_region.h);
#endif

		// FIXME using the clip area does not work with 
		// double buffering
		//SDL_SetClipRect(srf, &jive_dirty_region);
		jive_surface_set_clip(srf, NULL);

		/* Draw background */
		jive_tile_blit(jive_background, srf, 0, 0, screen_w, screen_h);

		if (perfwarn.screen) t3 = SDL_GetTicks();

		/* Draw screen */
		if (jive_getmethod(L, -2, "draw")) {
			lua_pushvalue(L, -3);	// widget
			lua_pushvalue(L, 2);	// surface
			lua_pushinteger(L, JIVE_LAYER_ALL); // layer
			lua_call(L, 3, 0);
		}
		jive_dirty_region.w = 0;

		/* Flip buffer */
		if (perfwarn.screen) t4 = SDL_GetTicks();
		jive_surface_flip(srf);
	}

	if (perfwarn.screen) {
		t5 = SDL_GetTicks();
		c1 = clock();
		if (t5-t0 > perfwarn.screen) {
			if (!t3) {
				t3 = t2; t4 = t2;
			}
			printf("update_screen > %dms: %4dms (%dms) [layout:%dms animate:%dms background:%dms draw:%dms flip:%dms]\n",
				   perfwarn.screen, t5-t0, (int)((c1-c0) * 1000 / CLOCKS_PER_SEC), t1-t0, t2-t1, t3-t2, t4-t3, t5-t4);
		}
	}
	
	lua_pop(L, 4);

	JIVEL_STACK_CHECK_END(L);

	return 0;
}


int jiveL_update_screen(lua_State *L) {
	/* stack is:
	 * 1: framework
	 */

	lua_pushcfunction(L, jive_traceback);  /* push traceback function */

	lua_pushcfunction(L, _update_screen);
	lua_pushvalue(L, 1);

	if (lua_pcall(L, 1, 0, 2) != 0) {
		fprintf(stderr, "error in event function:\n\t%s\n", lua_tostring(L, -1));
		return 0;
	}

	lua_pop(L, 1);

	return 0;
}


void jive_redraw(SDL_Rect *r) {
	if (jive_dirty_region.w) {
		jive_rect_union(&jive_dirty_region, r, &jive_dirty_region);
	}
	else {
		memcpy(&jive_dirty_region, r, sizeof(jive_dirty_region));
	}

	//printf("DIRTY: %d,%d %dx%d\n", jive_dirty_region.x, jive_dirty_region.y, jive_dirty_region.w, jive_dirty_region.h);
}


int jiveL_redraw(lua_State *L) {
	SDL_Rect r;

	/* stack top:
	 * -2: framework
	 * -1: rectangle or nil
	 */

	if (lua_isnil(L, -1)) {
		r.x = 0;
		r.y = 0;
		r.w = screen_w;
		r.h = screen_h;
	}
	else {
		jive_torect(L, 2, &r);
	}
	lua_pop(L, 1);

	jive_redraw(&r);

	return 0;
}


int jiveL_style_changed(lua_State *L) {

	/* stack top:
	 * 1: framework
	 */

	/* clear style cache */
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "jiveStyleCache");

	/* bump layout counter */
	next_jive_origin++;

	/* redraw screen */
	lua_pushcfunction(L, jiveL_redraw);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_call(L, 2, 0);

	return 0;
}


void jive_queue_event(JiveEvent *evt) {
	SDL_Event user_event;
	user_event.type = SDL_USEREVENT;

	user_event.user.code = JIVE_USER_EVENT_EVENT;
	user_event.user.data1 = malloc(sizeof(JiveEvent));
	memcpy(user_event.user.data1, evt, sizeof(JiveEvent));

	SDL_PushEvent(&user_event);
}


int jiveL_dispatch_event(lua_State *L) {
	Uint32 r = 0;
	Uint32 t0 = 0, t1 = 0;
	clock_t c0 = 0, c1 = 0;

	/* stack is:
	 * 1: framework
	 * 2: widget
	 * 3: event
	 */

	if (perfwarn.event) {
		t0 = SDL_GetTicks();
		c0 = clock();
	}

	lua_pushcfunction(L, jive_traceback);  /* push traceback function */

	// call global event listeners
	if (jive_getmethod(L, 1, "_event")) {
		lua_pushvalue(L, 1); // framework
		lua_pushvalue(L, 3); // event
		lua_pushboolean(L, 1); // global listeners

		if (lua_pcall(L, 3, 1, 4) != 0) {
			fprintf(stderr, "error in event function:\n\t%s\n", lua_tostring(L, -1));
			return 0;
		}

		r |= lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	/* by default send the event to the top window. fetch that top
	 * window here in case the global event handler has modified
	 * the window stack.
	 */
	if (lua_isnil(L, 2)) {
		lua_getfield(L, 1, "windowStack");
		if (lua_objlen(L, -1) == 0) {
			lua_pop(L, 1);
			return 0;
		}
		lua_rawgeti(L, -1, 1);
		lua_replace(L, 2);
	}

	// call widget event handler, unless the event is consumed
	if (!(r & JIVE_EVENT_CONSUME) && jive_getmethod(L, 2, "_event")) {
		lua_pushvalue(L, 2); // widget
		lua_pushvalue(L, 3); // event

		if (lua_pcall(L, 2, 1, 4) != 0) {
			fprintf(stderr, "error in event function:\n\t%s\n", lua_tostring(L, -1));
			return 0;
		}

		r |= lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	// call unused event listeners, unless the event is consumed
	if (!(r & JIVE_EVENT_CONSUME) && jive_getmethod(L, 1, "_event")) {
		lua_pushvalue(L, 1); // framework
		lua_pushvalue(L, 3); // event
		lua_pushboolean(L, 0); // unused listeners

		if (lua_pcall(L, 3, 1, 4) != 0) {
			fprintf(stderr, "error in event function:\n\t%s\n", lua_tostring(L, -1));
			return 0;
		}

		r |= lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	if (perfwarn.event) {
		t1 = SDL_GetTicks();
		c1 = clock();
		if (t1-t0 > perfwarn.event) {
			printf("process_event > %dms: %4dms (%dms) ", perfwarn.event, t1-t0, (int)((c1-c0) * 1000 / CLOCKS_PER_SEC));
			lua_getglobal(L, "tostring");
			lua_pushvalue(L, 2);
			lua_call(L, 1, 1);
			lua_pushcfunction(L, jiveL_event_tostring);
			lua_pushvalue(L, 3);
			lua_call(L, 1, 1);
			printf("[widget:%s event:%s]\n", lua_tostring(L, -2), lua_tostring(L, -1));
			lua_pop(L, 2);
		}
	}

	lua_pushinteger(L, r);
	return 1;
}

int jiveL_set_video_mode(lua_State *L) {
	JiveSurface *srf;
	Uint16 w, h, bpp;
	bool isfull;

	/* stack is:
	 * 1: framework
	 * 2: w
	 * 3: h
	 * 4: bpp
	 * 5: fullscreen
	 */

	w = luaL_optinteger(L, 2, 0);
	h = luaL_optinteger(L, 3, 0);
	bpp = luaL_optinteger(L, 4, 16);
	isfull = lua_toboolean(L, 5);

	if (w == screen_w &&
	    h == screen_h &&
	    bpp == screen_bpp &&
	    isfull == screen_isfull) {
		return 0;
	}


	/* update video mode */
	srf = jive_surface_set_video_mode(w, h, bpp, isfull);

	/* store new screen surface */
	lua_getfield(L, 1, "screen");
	tolua_pushusertype(L, srf, "Surface");
	lua_setfield(L, -2, "surface");

	lua_getfield(L, -1, "bounds");
	lua_pushvalue(L, 2); /* width */
	lua_rawseti(L, -2, 3);
	lua_pushvalue(L, 3); /* height */
	lua_rawseti(L, -2, 4);
	lua_pop(L, 2);

	screen_w = w;
	screen_h = h;
	screen_bpp = bpp;
	screen_isfull = isfull;

	next_jive_origin++;

	return 0;
}

int jiveL_get_background(lua_State *L) {
	tolua_pushusertype(L, jive_background, "Tile");
	return 1;
}

int jiveL_set_background(lua_State *L) {
	/* stack is:
	 * 1: framework
	 * 2: background image (tile)
	 */
	if (jive_background) {
		jive_tile_free(jive_background);
	}
	jive_background = jive_tile_ref(tolua_tousertype(L, 2, 0));
	next_jive_origin++;

	return 0;
}

int jiveL_push_event(lua_State *L) {

	/* stack is:
	 * 1: framework
	 * 2: JiveEvent
	 */

	JiveEvent *evt = lua_touserdata(L, 2);
	jive_queue_event(evt);

	return 0;
}

int jiveL_event(lua_State *L) {
	int r = 0;
	int listener_type;
	int event_type;

	/* stack is:
	 * 1: framework
	 * 2: event
	 * 3: globalListeners if true, or unusedListeners
	 */

	lua_getfield(L, 2, "getType");
	lua_pushvalue(L, 2);
	lua_call(L, 1, 1);
	event_type = lua_tointeger(L, -1);
	lua_pop(L, 1);

	listener_type = lua_toboolean(L, 3);
	if (listener_type) {
		lua_getfield(L, 1, "globalListeners");
	}
	else {
		lua_getfield(L, 1, "unusedListeners");
	}
	lua_pushnil(L);
	while (r == 0 && lua_next(L, -2) != 0) {
		int mask;

		lua_rawgeti(L, -1, 1);
		mask = lua_tointeger(L, -1);

		if (event_type & mask) {
			lua_rawgeti(L, -2, 2);
			lua_pushvalue(L, 2);
			lua_call(L, 1, 1);

			r = r | lua_tointeger(L, -1);

			lua_pop(L, 1);
		}

		lua_pop(L, 2);
	}
	lua_pop(L, 1);

	lua_pushinteger(L, r);
	return 1;
}


int jiveL_get_ticks(lua_State *L) {
	lua_pushinteger(L, SDL_GetTicks());
	return 1;
}


int jiveL_thread_time(lua_State *L) {
	lua_pushinteger(L, (int)(clock() * 1000 / CLOCKS_PER_SEC));
	return 1;
}


static Uint32 keyhold_callback(Uint32 interval, void *param) {
	SDL_Event user_event;
	memset(&user_event, 0, sizeof(SDL_Event));

	user_event.type = SDL_USEREVENT;
	user_event.user.code = JIVE_USER_EVENT_KEY_HOLD;
	user_event.user.data1 = param;

	SDL_PushEvent(&user_event);

	return 0;
}


static Uint32 mousehold_callback(Uint32 interval, void *param) {
	SDL_Event user_event;
	memset(&user_event, 0, sizeof(SDL_Event));

	user_event.type = SDL_USEREVENT;
	user_event.user.code = JIVE_USER_EVENT_MOUSE_HOLD;
	user_event.user.data1 = param;

	SDL_PushEvent(&user_event);

	return 0;
}

static Uint32 pointer_callback(Uint32 interval, void *param) {
	SDL_ShowCursor(SDL_DISABLE);

	return 0;
}


static int do_dispatch_event(lua_State *L, JiveEvent *jevent) {
	int r;

	/* Send event to lua widgets */
	r = JIVE_EVENT_UNUSED;
	lua_pushcfunction(L, jiveL_dispatch_event);
	jiveL_getframework(L);
	lua_pushnil(L); // default to top window
	jive_pushevent(L, jevent);
	lua_call(L, 3, 1);
	r = lua_tointeger(L, -1);
	lua_pop(L, 1);

	return r;
}


static int process_event(lua_State *L, SDL_Event *event) {
	JiveEvent jevent;

	memset(&jevent, 0, sizeof(JiveEvent));
	jevent.ticks = SDL_GetTicks();

	switch (event->type) {
	case SDL_QUIT:
		jiveL_quit(L);
		exit(0);
		break;

	case SDL_MOUSEBUTTONDOWN:
		/* map the mouse scroll wheel to up/down */
		if (event->button.button == SDL_BUTTON_WHEELUP) {
			jevent.type = JIVE_EVENT_SCROLL;
			--(jevent.u.scroll.rel);
			break;
		}
		else if (event->button.button == SDL_BUTTON_WHEELDOWN) {
			jevent.type = JIVE_EVENT_SCROLL;
			++(jevent.u.scroll.rel);
			break;
		}

		// Fall through

	case SDL_MOUSEBUTTONUP:
		if (event->button.button == SDL_BUTTON_LEFT) {
			if (event->button.state == SDL_PRESSED) {
				jevent.type = JIVE_EVENT_MOUSE_DOWN;
				jevent.u.mouse.x = event->button.x;
				jevent.u.mouse.y = event->button.y;
			}
			else {
				jevent.type = JIVE_EVENT_MOUSE_UP;
				jevent.u.mouse.x = event->button.x;
				jevent.u.mouse.y = event->button.y;
			}
		}

		if (event->type == SDL_MOUSEBUTTONDOWN) {
			if (mouse_state == MOUSE_STATE_NONE) {
				unsigned int * param = malloc(sizeof(unsigned int));

				mouse_state = MOUSE_STATE_DOWN;

				if (mouse_timer) {
					SDL_RemoveTimer(mouse_timer);
				}

				*param = (event->button.y << 16) | event->button.x;
				mouse_timer = SDL_AddTimer(HOLD_TIMEOUT, &mousehold_callback, param);

				mouse_origin_x = event->button.x;
				mouse_origin_y = event->button.y;
				break;
			}
		}
		else /* SDL_MOUSEBUTTONUP */ {
			if (mouse_state == MOUSE_STATE_DOWN) {
				/*
				 * MOUSE_PRESSED and MOUSE_UP events
				 */
				JiveEvent up;

				memset(&up, 0, sizeof(JiveEvent));
				up.type = JIVE_EVENT_MOUSE_PRESS;
				up.ticks = SDL_GetTicks();
				up.u.mouse.x = event->button.x;
				up.u.mouse.y = event->button.y;
				jive_queue_event(&up);
			}

			if (mouse_timer) {
				SDL_RemoveTimer(mouse_timer);
				mouse_timer = NULL;
			}

			mouse_state = MOUSE_STATE_NONE;
		}
		break;

	case SDL_MOUSEMOTION:

		/* show mouse cursor */
		if (pointer_timer) {
			SDL_RemoveTimer(pointer_timer);
		}
		SDL_ShowCursor(SDL_ENABLE);
		SDL_AddTimer(POINTER_TIMEOUT, &pointer_callback, NULL);

		if (event->motion.state & SDL_BUTTON(1)) {
			if ( ( (mouse_state == MOUSE_STATE_DOWN || mouse_state == MOUSE_STATE_SENT)
			       && (abs(mouse_origin_x - event->motion.x) > 10
				   || abs(mouse_origin_y - event->motion.y) > 10))
			     || mouse_state == MOUSE_STATE_DRAG) {
				jevent.type = JIVE_EVENT_MOUSE_DRAG;
				jevent.u.mouse.x = event->motion.x;
				jevent.u.mouse.y = event->motion.y;

				mouse_state = MOUSE_STATE_DRAG;
			}
		}
		else {
			jevent.type = JIVE_EVENT_MOUSE_MOVE;
			jevent.u.mouse.x = event->motion.x;
			jevent.u.mouse.y = event->motion.y;
		}
		break;

	case SDL_KEYDOWN:
		if (event->key.keysym.sym == SDLK_UP) {
			jevent.type = JIVE_EVENT_SCROLL;
			--(jevent.u.scroll.rel);
			break;
		}
		else if (event->key.keysym.sym == SDLK_DOWN) {
			jevent.type = JIVE_EVENT_SCROLL;
			++(jevent.u.scroll.rel);
			break;
		}
		// Fall through

	case SDL_KEYUP: {
		struct jive_keymap *entry = keymap;
		
		while (entry->keysym != SDLK_UNKNOWN) {
			if (entry->keysym == event->key.keysym.sym) {
				break;
			}
			entry++;
		}

		if (entry->keysym == SDLK_UNKNOWN) {
			// handle regular character keys ('a', 't', etc..)
			if (event->type == SDL_KEYDOWN && event->key.keysym.unicode != 0) {
				JiveEvent textEvent;

				memset(&textEvent, 0, sizeof(JiveEvent));
				textEvent.type = JIVE_EVENT_CHAR_PRESS;
				textEvent.ticks = SDL_GetTicks();
				if (event->key.keysym.sym == SDLK_BACKSPACE) {
					//special case for Backspace, where value set is not ascii value, instead pass backspace ascii value
					textEvent.u.text.unicode = 8;
				} else {
					textEvent.u.text.unicode = event->key.keysym.unicode;
				}
				jive_queue_event(&textEvent);
			}
			return 0;
		}

		/* handle pgup/upgn as repeatable keys */
		if (entry->keysym == SDLK_PAGEUP || entry->keysym == SDLK_PAGEDOWN) {
			if (event->type == SDL_KEYDOWN) {
				JiveEvent keypress;
	
				memset(&keypress, 0, sizeof(JiveEvent));
				keypress.type = JIVE_EVENT_KEY_PRESS;
				keypress.ticks = SDL_GetTicks();
				keypress.u.key.code = entry->keycode;
				jive_queue_event(&keypress);
			}
			return 0;
		}
		
		if (event->type == SDL_KEYDOWN) {
			if (key_mask & entry->keycode) {
				// ignore key repeats
				return 0;
			}
			if (key_mask == 0) {
				key_state = KEY_STATE_NONE;
			}

			switch (key_state) {
			case KEY_STATE_NONE:
				key_state = KEY_STATE_DOWN;
				// fall through

			case KEY_STATE_DOWN: {
				key_mask |= entry->keycode;

				jevent.type = JIVE_EVENT_KEY_DOWN;
				jevent.u.key.code = entry->keycode;

				if (key_timer) {
					SDL_RemoveTimer(key_timer);
				}

				key_timer = SDL_AddTimer(HOLD_TIMEOUT, &keyhold_callback, (void *)key_mask);
				break;
			 }

			case KEY_STATE_SENT:
				break;
			}
		}
		else /* SDL_KEYUP */ {
			if (! (key_mask & entry->keycode)) {
				// ignore repeated key ups
				return 0;
			}

			switch (key_state) {
			case KEY_STATE_NONE:
				break;

			case KEY_STATE_DOWN: {
				/*
				 * KEY_PRESSED and KEY_UP events
				 */
				JiveEvent keyup;

				jevent.type = JIVE_EVENT_KEY_PRESS;
				jevent.u.key.code = key_mask;

				memset(&keyup, 0, sizeof(JiveEvent));
				keyup.type = JIVE_EVENT_KEY_UP;
				keyup.ticks = SDL_GetTicks();
				keyup.u.key.code = entry->keycode;
				jive_queue_event(&keyup);

				key_state = KEY_STATE_SENT;
				break;
			}

			case KEY_STATE_SENT: {
				/*
				 * KEY_UP event
				 */
				jevent.type = JIVE_EVENT_KEY_UP;
				jevent.u.key.code = entry->keycode;
				break;
			}
			}

			if (key_timer) {
				SDL_RemoveTimer(key_timer);
				key_timer = NULL;
			}

			key_mask &= ~(entry->keycode);
			if (key_mask == 0) {
				key_state = KEY_STATE_NONE;
			}
		}
		break;
	}

	case SDL_USEREVENT:
		switch ( (int) event->user.code) {
		case JIVE_USER_EVENT_TIMER:
			JIVEL_STACK_CHECK_BEGIN(L);
			jive_timer_dispatch_event(L, event->user.data1);
			JIVEL_STACK_CHECK_END(L);
			return 0;

		case JIVE_USER_EVENT_KEY_HOLD:
			jevent.type = JIVE_EVENT_KEY_HOLD;
			jevent.u.key.code = (JiveKey) event->user.data1;
			key_state = KEY_STATE_SENT;
			break;

		case JIVE_USER_EVENT_MOUSE_HOLD:
			if (mouse_state == MOUSE_STATE_DOWN) {
				jevent.type = JIVE_EVENT_MOUSE_HOLD;
				jevent.u.mouse.x = *((unsigned int *) event->user.data1) & 0xFFFF;
				jevent.u.mouse.y = ( *((unsigned int *) event->user.data1) >> 16) & 0xFFFF;
				mouse_state = MOUSE_STATE_SENT;
				free(event->user.data1);
			}
			break;

		case JIVE_USER_EVENT_EVENT:
			memcpy(&jevent, event->user.data1, sizeof(JiveEvent));
			free(event->user.data1);
			break;
		}
		break;

	case SDL_VIDEORESIZE: {
		JiveSurface *srf;
		int bpp = 16;

		screen_w = event->resize.w;
		screen_h = event->resize.h;

		srf = jive_surface_set_video_mode(screen_w, screen_h, bpp, false);

		lua_getfield(L, 1, "screen");

		lua_getfield(L, -1, "bounds");
		lua_pushinteger(L, screen_w);
		lua_rawseti(L, -2, 3);
		lua_pushinteger(L, screen_h);
		lua_rawseti(L, -2, 4);
		lua_pop(L, 1);

		tolua_pushusertype(L, srf, "Surface");
		lua_setfield(L, -2, "surface");

		lua_pop(L, 1);

		next_jive_origin++;

		jevent.type = JIVE_EVENT_WINDOW_RESIZE;
		
		/* Avoid mouse_up causing a mouse press event to occur */
		mouse_state = MOUSE_STATE_NONE;
		break;

	}

	default:
		return 0;
	}

	return do_dispatch_event(L, &jevent);
}


int jiveL_perfwarn(lua_State *L) {
	/* stack is:
	 * 1: framework
	 * 2: table of threshold values, if no entry or 0 warnings are disabled 
	 */

	if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "screen");
		perfwarn.screen = lua_tointeger(L, -1);
		lua_getfield(L, 2, "layout");
		perfwarn.layout = lua_tointeger(L, -1);
		lua_getfield(L, 2, "draw");
		perfwarn.draw = lua_tointeger(L, -1);
		lua_getfield(L, 2, "event");
		perfwarn.event = lua_tointeger(L, -1);
		lua_getfield(L, 2, "queue");
		perfwarn.queue = lua_tointeger(L, -1);
		lua_getfield(L, 2, "garbage");
		perfwarn.garbage = lua_tointeger(L, -1);
		lua_pop(L, 6);
	}
	
	return 0;
}


static const struct luaL_Reg icon_methods[] = {
	{ "getPreferredBounds", jiveL_icon_get_preferred_bounds },
	{ "setValue", jiveL_icon_set_value },
	{ "_skin", jiveL_icon_skin },
	{ "_layout", jiveL_icon_layout },
	{ "draw", jiveL_icon_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg label_methods[] = {
	{ "getPreferredBounds", jiveL_label_get_preferred_bounds },
	{ "_skin", jiveL_label_skin },
	{ "_layout", jiveL_label_layout },
	{ "animate", jiveL_label_animate },
	{ "draw", jiveL_label_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg group_methods[] = {
	{ "getPreferredBounds", jiveL_group_get_preferred_bounds },
	{ "_skin", jiveL_group_skin },
	{ "_layout", jiveL_group_layout },
	{ "iterate", jiveL_group_iterate },
	{ "draw", jiveL_group_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg textinput_methods[] = {
	{ "getPreferredBounds", jiveL_textinput_get_preferred_bounds },
	{ "_skin", jiveL_textinput_skin },
	{ "_layout", jiveL_textinput_layout },
	{ "draw", jiveL_textinput_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg menu_methods[] = {
	{ "_skin", jiveL_menu_skin },
	{ "_layout", jiveL_menu_layout },
	{ "iterate", jiveL_menu_iterate },
	{ "draw", jiveL_menu_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg slider_methods[] = {
	{ "getPreferredBounds", jiveL_slider_get_preferred_bounds },
	{ "_skin", jiveL_slider_skin },
	{ "_layout", jiveL_slider_layout },
	{ "draw", jiveL_slider_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg textarea_methods[] = {
	{ "getPreferredBounds", jiveL_textarea_get_preferred_bounds },
	{ "_skin", jiveL_textarea_skin },
	{ "_layout", jiveL_textarea_layout },
	{ "draw", jiveL_textarea_draw },
	{ NULL, NULL }
};

static const struct luaL_Reg widget_methods[] = {
	{ "setBounds", jiveL_widget_set_bounds }, 
	{ "getBounds", jiveL_widget_get_bounds },
	{ "getPreferredBounds", jiveL_widget_get_preferred_bounds },
	{ "getBorder", jiveL_widget_get_border },
	{ "mouseBounds", jiveL_widget_mouse_bounds },
	{ "mouseInside", jiveL_widget_mouse_inside },
	{ "reSkin", jiveL_widget_reskin },
	{ "reLayout", jiveL_widget_relayout },
	{ "reDraw", jiveL_widget_redraw },
	{ "checkSkin", jiveL_widget_check_skin },
	{ "checkLayout", jiveL_widget_check_layout },
	{ "peerToString", jiveL_widget_peer_tostring },
	{ "stylePath", jiveL_style_path },
	{ "styleValue", jiveL_style_value },
	{ "styleInt", jiveL_style_value },
	{ "styleColor", jiveL_style_color },
	{ "styleImage", jiveL_style_value },
	{ "styleFont", jiveL_style_font },
	{ NULL, NULL }
};

static const struct luaL_Reg window_methods[] = {
	{ "_skin", jiveL_window_skin },
	{ "checkLayout", jiveL_window_check_layout },
	{ "iterate", jiveL_window_iterate },
	{ "draw", jiveL_window_draw },
	{ "_eventHandler", jiveL_window_event_handler },
	{ NULL, NULL }
};

static const struct luaL_Reg timer_methods[] = {
	{ "start", jiveL_timer_add_timer },
	{ "stop", jiveL_timer_remove_timer },
	{ NULL, NULL }
};

static const struct luaL_Reg event_methods[] = {
	{ "new", jiveL_event_new },
	{ "getType", jiveL_event_get_type },
	{ "getTicks", jiveL_event_get_ticks },
	{ "getScroll", jiveL_event_get_scroll },
	{ "getKeycode", jiveL_event_get_keycode },
	{ "getUnicode", jiveL_event_get_unicode },
	{ "getMouse", jiveL_event_get_mouse },
	{ "getMotion", jiveL_event_get_motion },
	{ "getSwitch", jiveL_event_get_switch },
	{ "tostring", jiveL_event_tostring },
	{ NULL, NULL }
};

static const struct luaL_Reg core_methods[] = {
	{ "init", jiveL_init },
	{ "quit", jiveL_quit },
	{ "processEvents", jiveL_process_events },
	{ "setUpdateScreen", jiveL_set_update_screen },
	{ "updateScreen", jiveL_update_screen },
	{ "reDraw", jiveL_redraw },
	{ "pushEvent", jiveL_push_event },
	{ "dispatchEvent", jiveL_dispatch_event },
	{ "getTicks", jiveL_get_ticks },
	{ "threadTime", jiveL_thread_time },
	{ "setVideoMode", jiveL_set_video_mode },
	{ "getBackground", jiveL_get_background },
	{ "setBackground", jiveL_set_background },
	{ "styleChanged", jiveL_style_changed },
	{ "perfwarn", jiveL_perfwarn },
	{ "_event", jiveL_event },
	{ NULL, NULL }
};



static int jiveL_core_init(lua_State *L) {

	squeezeplayL_system_init(L);

	lua_getglobal(L, "jive");
	lua_getfield(L, -1, "ui");

	/* stack is:
	 * 1: jive table
	 * 2: ui table
	 */

	/* register methods */
	lua_getfield(L, 2, "Icon");
	luaL_register(L, NULL, icon_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Label");
	luaL_register(L, NULL, label_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Group");
	luaL_register(L, NULL, group_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Textinput");
	luaL_register(L, NULL, textinput_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Menu");
	luaL_register(L, NULL, menu_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Textarea");
	luaL_register(L, NULL, textarea_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Widget");
	luaL_register(L, NULL, widget_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Window");
	luaL_register(L, NULL, window_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Slider");
	luaL_register(L, NULL, slider_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Timer");
	luaL_register(L, NULL, timer_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Event");
	luaL_register(L, NULL, event_methods);
	lua_pop(L, 1);

	lua_getfield(L, 2, "Framework");
	luaL_register(L, NULL, core_methods);
	lua_pop(L, 1);
	
	return 0;
}

static const struct luaL_Reg core_funcs[] = {
	{ "frameworkOpen", jiveL_core_init },
	{ NULL, NULL }
};

int luaopen_jive_ui_framework(lua_State *L) {
	luaL_register(L, "jive", core_funcs);
	return 1;
}
