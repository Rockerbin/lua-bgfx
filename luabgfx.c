#define LUA_LIB

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <bgfx/c99/bgfx.h>
#include <assert.h>

#include "luabgfx.h"
#include "simplelock.h"
#include "bgfx_interface.h"
#include "bgfx_alloc.h"

#if BGFX_API_VERSION != 108
#   error BGFX_API_VERSION mismatch
#endif

#if _MSC_VER > 0
#include <malloc.h>

#	ifdef USING_ALLOCA_FOR_VLA
#		define VLA(_TYPE, _VAR, _SIZE)	_TYPE _VAR = (_TYPE*)_alloca(sizeof(_TYPE) * (_SIZE))
#	else//!USING_ALLOCA_FOR_VLA
#		define V(_SIZE)	4096
#	endif //USING_ALLOCA_FOR_VLA
#else //!(_MSC_VER > 0)
#	ifdef USING_ALLOCA_FOR_VLA
#		define VLA(_TYPE, _VAR, _SIZE) _TYPE _VAR[(_SIZE)]
#	else //!USING_ALLOCA_FOR_VLA
#		define V(_SIZE)	(_SIZE)
#	endif //USING_ALLOCA_FOR_VLA

#endif //_MSC_VER > 0

#ifndef lua_newuserdata
// lua 5.4

static void *
lua_newuserdatauv(lua_State *L, size_t size, int nuvalue) {
	if (nuvalue > 1)
		luaL_error(L, "Don't support nuvalue (%d) > 1", nuvalue);
	return lua_newuserdata(L, size);
}

#endif


// screenshot queue length
#define MAX_SCREENSHOT 16
// 64K log ring buffer
#define MAX_LOGBUFFER (64*1024)

struct screenshot {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t size;
	void *data;
	char *name;
};

struct screenshot_queue {
	spinlock_t lock;
	unsigned int head;
	unsigned int tail;
	struct screenshot *q[MAX_SCREENSHOT];
};

struct log_cache {
	spinlock_t lock;
	uint64_t head;
	uint64_t tail;
	char log[MAX_LOGBUFFER];
};

struct callback {
	bgfx_callback_interface_t base;
	struct screenshot_queue ss;
	struct log_cache lc;
	bool getlog;
};

struct memory {
	void *data;
	size_t size;
	int ref;
	int constant;
};

static int
memory_tostring(lua_State *L) {
	struct memory *mem = (struct memory *)lua_touserdata(L, 1);
	if (mem->constant) {
		lua_getuservalue(L, 1);
		size_t sz;
		const char *str = lua_tolstring(L, -1, &sz);
		if (str == (const char *)mem->data && sz == mem->size) {
			return 1;
		}
	}
	lua_pushlstring(L, mem->data, mem->size);
	return 1;
}

// base 1
static int
memory_read(lua_State *L) {
	struct memory *mem = (struct memory *)lua_touserdata(L, 1);
	int index = luaL_checkinteger(L, 2)-1;
	if (index < 0 || index >= mem->size) {
		return 0;
	}
	uint8_t * data = (uint8_t *)mem->data;
	lua_pushinteger(L, data[index]);
	return 1;
}

static int
memory_write(lua_State *L) {
	struct memory *mem = (struct memory *)lua_touserdata(L, 1);
	if (mem->constant) {
		return luaL_error(L, "Can't write to constant memory object");
	}
	int index = luaL_checkinteger(L, 2)-1;
	if (index < 0 || index >= mem->size) {
		return luaL_error(L, "out of range %d/[1-%d]", index+1, mem->size / sizeof(uint32_t));
	}
	uint32_t v = luaL_checkinteger(L, 3);
	uint8_t * data = (uint8_t *)mem->data;
	data[index] = v;
	return 0;
}

static int
memory_keepalive(lua_State *L) {
	lua_getuservalue(L, 1);
	struct memory *mem = (struct memory *)lua_touserdata(L, -1);
	if (mem->ref == 0)
		return 0;
	// keep alive
	lua_newuserdata(L, 0);
	luaL_getmetatable(L, "BGFX_MEMORY_REF");
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -2);
	lua_setuservalue(L, -2);
	return 0;
}

static int
memory_release(lua_State *L) {
	struct memory *mem = (struct memory *)lua_touserdata(L, 1);
	if (mem->ref == 0)
		return 0;
	// keep self alive
	lua_newuserdata(L, 0);
	if (luaL_newmetatable(L, "BGFX_MEMORY_REF")) {
		lua_pushcfunction(L, memory_keepalive);
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);
	lua_pushvalue(L, 1);
	lua_setuservalue(L, -2);

	return 0;
}

static struct memory *
memory_new(lua_State *L) {
	struct memory *mem = (struct memory *)lua_newuserdata(L, sizeof(*mem));
	mem->data = NULL;
	mem->size = 0;
	mem->ref = 0;
	mem->constant = 0;
	if (luaL_newmetatable(L, "BGFX_MEMORY")) {
		luaL_Reg l[] = {
			{ "__tostring", memory_tostring },
			{ "__index", memory_read },
			{ "__newindex", memory_write },
			{ "__gc", memory_release },
			{ NULL, NULL },
		};
		luaL_setfuncs(L, l, 0);
	}
	lua_setmetatable(L, -2);
	return mem;
}

// pop data (data != NULL) from stack, and push memory object on the stack
static void *
newMemory(lua_State *L, void *data, size_t size) {
	if (data == NULL) {
		data = lua_newuserdatauv(L, size, 0);
	}
	struct memory *mem = memory_new(L);
	lua_insert(L, -2);
	if (lua_type(L, -1) == LUA_TSTRING) {
		mem->constant = 1;
	}
	lua_setuservalue(L, -2);
	mem->data = data;
	mem->size = size;

	return data;
}

static void
releaseMemory(void *ptr, void *ud) {
	(void)ptr;	// unused ptr
	struct memory * mem = (struct memory *)ud;
	atom_dec(&mem->ref);
}

static const bgfx_memory_t *
bgfxMemory(lua_State *L, int idx) {
	struct memory *mem = (struct memory *)luaL_checkudata(L, idx, "BGFX_MEMORY");
	atom_inc(&mem->ref);
	return BGFX(make_ref_release)(mem->data, mem->size, releaseMemory, (void *)mem);
}

static void *
getfield(lua_State *L, const char *key) {
	lua_getfield(L, 1, key);
	void * ud = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ud;
}

static int
lsetPlatformData(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	bgfx_platform_data_t bpdt;
	memset(&bpdt, 0, sizeof(bpdt));

	bpdt.ndt = getfield(L, "ndt");
	bpdt.nwh = getfield(L, "nwh");
	bpdt.context = getfield(L, "context");
	bpdt.backBuffer = getfield(L, "backBuffer");
	bpdt.backBufferDS = getfield(L, "backBufferDS");
	
	BGFX(set_platform_data)(&bpdt);

	return 0;
}

static bgfx_renderer_type_t
renderer_type_id(lua_State *L, int index) {
	const char * type = luaL_checkstring(L, index);
	bgfx_renderer_type_t id;
#define RENDERER_TYPE_ID(x) else if (strcmp(type, #x) == 0) id = BGFX_RENDERER_TYPE_##x
	if (0) ;
	RENDERER_TYPE_ID(NOOP);
	RENDERER_TYPE_ID(DIRECT3D9);
	RENDERER_TYPE_ID(DIRECT3D11);
	RENDERER_TYPE_ID(DIRECT3D12);
	RENDERER_TYPE_ID(GNM);
	RENDERER_TYPE_ID(METAL);
	RENDERER_TYPE_ID(NVN);
	RENDERER_TYPE_ID(OPENGLES);
	RENDERER_TYPE_ID(OPENGL);
	RENDERER_TYPE_ID(VULKAN);
	else return luaL_error(L, "Invalid renderer type %s", type);

	return id;
}

static void
append_log(struct log_cache *lc, const char * buffer, int n) {
	spin_lock(lc);
	int sz = (int)(lc->tail - lc->head);	// sz must less than MAX_LOGBUFFER
	if (sz + n > MAX_LOGBUFFER)
		n = MAX_LOGBUFFER - sz;
	int offset = lc->tail % MAX_LOGBUFFER;
	int part = MAX_LOGBUFFER - offset;
	if (part >= n) {
		// only one part
		memcpy(lc->log + offset, buffer, n);
	} else {
		// ring buffer rewind
		memcpy(lc->log + offset, buffer, part);
		memcpy(lc->log, buffer + part, n - part);
	}
	lc->tail += n;
	spin_unlock(lc);
}

static const char*
fatal_code_str(bgfx_fatal_t code) {
	switch (code) {
	case BGFX_FATAL_DEBUG_CHECK:
		return "DebugCheck";
	case BGFX_FATAL_INVALID_SHADER:
		return "InvalidShader";
	case BGFX_FATAL_UNABLE_TO_INITIALIZE:
		return "UnableToInitialize";
	case BGFX_FATAL_UNABLE_TO_CREATE_TEXTURE:
		return "UnableToCreateTexture";
	case BGFX_FATAL_DEVICE_LOST:
		return "DeviceLost";
	default:
		return "Unknown";
	}
}

static void
cb_fatal(bgfx_callback_interface_t *self, const char* filePath, uint16_t line, bgfx_fatal_t code, const char *str) {
	fprintf(stderr, "Fatal error at %s(%d): [%s] %s\n", filePath, line, fatal_code_str(code), str);
	fflush(stderr);
	abort();
}

static void
cb_trace_vargs(bgfx_callback_interface_t *self, const char *file, uint16_t line, const char *format, va_list ap) {
	char tmp[MAX_LOGBUFFER];
	int n = sprintf(tmp, "%s (%d): ", file, line);

	n += vsnprintf(tmp+n, sizeof(tmp)-n, format, ap);

	if (n > MAX_LOGBUFFER) {
		// truncated
		n = MAX_LOGBUFFER;
	}
	struct callback * cb = (struct callback *)self;
	if (cb->getlog) {
		append_log(&(cb->lc), tmp, n);
	} else {
		fputs(tmp, stdout);
	}
}

// todo: bgfx callback

static void
cb_profiler_begin(bgfx_callback_interface_t *self, const char* _name, uint32_t _abgr, const char* _filePath, uint16_t _line) {
}


static void
cb_profiler_begin_literal(bgfx_callback_interface_t *self, const char* _name, uint32_t _abgr, const char* _filePath, uint16_t _line) {
}

static void
cb_profiler_end(bgfx_callback_interface_t *self) {
}

static uint32_t
cb_cache_read_size(bgfx_callback_interface_t *self, uint64_t id) {
	return 0;
}

static bool
cb_cache_read(bgfx_callback_interface_t *self,  uint64_t id, void *data, uint32_t size) {
	return false;
}

static void
cb_cache_write(bgfx_callback_interface_t *self, uint64_t id, const void *data, uint32_t size) {
}

static struct screenshot *
ss_pop(struct screenshot_queue *queue) {
	struct screenshot *r;
	spin_lock(queue);
	if (queue->head == queue->tail) {
		r = NULL;
	} else {
		r = queue->q[queue->tail % MAX_SCREENSHOT];
		++queue->tail;
	}
	spin_unlock(queue);
	return r;
}

// succ return NULL
static struct screenshot *
ss_push(struct screenshot_queue *queue, struct screenshot *s) {
	spin_lock(queue);
	if ((queue->head - queue->tail) != MAX_SCREENSHOT) {
		queue->q[queue->head % MAX_SCREENSHOT] = s;
		++queue->head;
		s = NULL;
	}
	spin_unlock(queue);
	return s;
}

static void
ss_free(struct screenshot * s) {
	if (s == NULL)
		return;
	free(s->name);
	free(s->data);
	free(s);
}

static void
cb_screen_shot(bgfx_callback_interface_t *self, const char* file, uint32_t width, uint32_t height, uint32_t pitch, const void* data, uint32_t size, bool yflip) {
	struct screenshot *s = malloc(sizeof(*s));
	size_t fn_sz = strlen(file);
	s->name = malloc(fn_sz + 1);
	memcpy(s->name, file, fn_sz+1);
	s->data = malloc(size);
	s->width = width;
	s->height = height;
	s->pitch = pitch;
	s->size = size;
	uint8_t * dst = s->data;
	const uint8_t * src = (const uint8_t *)data;
	int line_pitch = pitch;
	if (yflip) {
		src += (height-1) * pitch;
		line_pitch = - (int32_t)pitch;
	}
	uint32_t pixwidth = size / height;
	uint32_t i;
	for (i=0;i<height;i++) {
		memcpy(dst, src, pixwidth);
		dst += pixwidth;
		src += line_pitch;
	}
	struct callback *cb = (struct callback *)self;
	s = ss_push(&cb->ss, s);
	ss_free(s);
}

static void
cb_capture_begin(bgfx_callback_interface_t *self, uint32_t width, uint32_t height, uint32_t pitch, bgfx_texture_format_t format, bool yflip) {
}

static void
cb_capture_end(bgfx_callback_interface_t *self) {
}

static void
cb_capture_frame(bgfx_callback_interface_t* self, const void* _data, uint32_t _size) {
}

static uint32_t
reset_flags(lua_State *L, int index) {
	const char * flags = lua_tostring(L, index);
	uint32_t f = BGFX_RESET_NONE;
	if (flags) {
		int i;
		for (i=0;flags[i];i++) {
			switch(flags[i]) {
			case 'f' : f |= BGFX_RESET_FULLSCREEN; break;
			case 'v' : f |= BGFX_RESET_VSYNC; break;
			case 'a' : f |= BGFX_RESET_MAXANISOTROPY; break;
			case 'c' : f |= BGFX_RESET_CAPTURE; break;
			case 'u' : f |= BGFX_RESET_FLUSH_AFTER_RENDER; break;
			case 'i' : f |= BGFX_RESET_FLIP_AFTER_RENDER; break;
			case 's' : f |= BGFX_RESET_SRGB_BACKBUFFER; break;
			case 'm' :
				++i;
				switch (flags[i]) {
				case '2' : f |= BGFX_RESET_MSAA_X2; break;
				case '4' : f |= BGFX_RESET_MSAA_X4; break;
				case '8' : f |= BGFX_RESET_MSAA_X8; break;
				case 'x' : f |= BGFX_RESET_MSAA_X16; break;
				default:
					return luaL_error(L, "Invalid MSAA %c", flags[i]);
				}
				break;
			case 'h': f |= BGFX_RESET_HDR10; break;
			case 'p': f |= BGFX_RESET_HIDPI; break;
			case 'd': f |= BGFX_RESET_DEPTH_CLAMP; break;
			case 'z': f |= BGFX_RESET_SUSPEND; break;
			default:
				return luaL_error(L, "Invalid reset flag %c", flags[i]);
			}
		}
	}
	return f;
}

static void
read_uint32(lua_State *L, int index, const char * key, uint32_t *v) {
	if (lua_getfield(L, index, key) != LUA_TNIL) {
		*v = luaL_checkinteger(L, -1);
	}
	lua_pop(L, 1);
}

static void
read_boolean(lua_State *L, int index, const char *key, bool *v) {
	int t = lua_getfield(L, index, key);
	if (t == LUA_TBOOLEAN || t == LUA_TNIL) {
		*v = lua_toboolean(L, -1);
	} else {
		luaL_error(L, ".%s need boolean, it's %s", key, lua_typename(L, t));
	}
	lua_pop(L, 1);
}

static bgfx_texture_format_t
texture_format_from_string(lua_State *L, int idx) {
	lua_getfield(L, LUA_REGISTRYINDEX, "BGFX_TF");
	lua_pushvalue(L, idx);
	if (lua_rawget(L, -2) != LUA_TNUMBER) {
		luaL_error(L, "Invalid texture format %s", lua_tostring(L, idx));
	}
	int id = lua_tointeger(L, -1);
	lua_pop(L, 2);
	return (bgfx_texture_format_t)id;
}

static int
linit(lua_State *L) {
	static bgfx_callback_vtbl_t vtbl = {
		cb_fatal,
		cb_trace_vargs,
		cb_profiler_begin,
		cb_profiler_begin_literal,
		cb_profiler_end,
		cb_cache_read_size,
		cb_cache_read,
		cb_cache_write,
		cb_screen_shot,
		cb_capture_begin,
		cb_capture_end,
		cb_capture_frame,
	};
	struct callback *cb = lua_newuserdata(L, sizeof(*cb));
	memset(cb, 0, sizeof(*cb));
	lua_setfield(L, LUA_REGISTRYINDEX, "bgfx_cb");
	cb->base.vtbl = &vtbl;

	bgfx_init_t init;

	init.type = BGFX_RENDERER_TYPE_COUNT;
	init.vendorId = BGFX_PCI_ID_NONE;
	init.deviceId = 0;

	init.resolution.format = BGFX_TEXTURE_FORMAT_RGBA8;
	init.resolution.width = 1280;
	init.resolution.height = 720;
	init.resolution.reset = BGFX_RESET_NONE;	// reset flags
	init.resolution.numBackBuffers = 2;
	init.resolution.maxFrameLatency = 0;

	init.limits.maxEncoders     = 8;	// BGFX_CONFIG_DEFAULT_MAX_ENCODERS;
	init.limits.minResourceCbSize = (64<<10); // BGFX_CONFIG_MIN_RESOURCE_COMMAND_BUFFER_SIZE
	init.limits.transientVbSize = (6<<20);	// BGFX_CONFIG_TRANSIENT_VERTEX_BUFFER_SIZE
	init.limits.transientIbSize = (2<<20);	// BGFX_CONFIG_TRANSIENT_INDEX_BUFFER_SIZE;

	init.callback = &cb->base;
	init.allocator = NULL;
	init.debug = false;
	init.profile = false;

	init.platformData.ndt = NULL;
	init.platformData.nwh = NULL;
	init.platformData.context = NULL;
	init.platformData.backBuffer = NULL;
	init.platformData.backBufferDS = NULL;

	if (!lua_isnoneornil(L, 1)) {
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_settop(L, 1);
		if (lua_getfield(L, 1, "renderer") == LUA_TSTRING) {
			init.type = renderer_type_id(L, 2);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "format") == LUA_TSTRING) {
			init.resolution.format = texture_format_from_string(L, 2);
		}
		lua_pop(L, 1);
		read_uint32(L, 1, "width", &init.resolution.width);
		read_uint32(L, 1, "height", &init.resolution.height);
		if (lua_getfield(L, 1, "reset") == LUA_TSTRING) {
			init.resolution.reset = reset_flags(L, -1);
		}

		lua_getfield(L, 1, "numBackBuffers");
		init.resolution.numBackBuffers = luaL_optinteger(L, -1, 2);
		lua_pop(L, 1);

		lua_getfield(L, 1, "maxFrameLatency");
		init.resolution.maxFrameLatency = luaL_optinteger(L, -1, 0);
		lua_pop(L, 1);

		if (lua_getfield(L, 1, "maxEncoders") == LUA_TNUMBER) {
			init.limits.maxEncoders = luaL_checkinteger(L, -1);
		}
		lua_pop(L, 1);
		read_uint32(L, 1, "minResourceCbSize", &init.limits.minResourceCbSize);
		read_uint32(L, 1, "transientVbSize", &init.limits.transientVbSize);
		read_uint32(L, 1, "transientIbSize", &init.limits.transientIbSize);
		read_boolean(L, 1, "debug", &init.debug);
		read_boolean(L, 1, "profile", &init.profile);
		read_boolean(L, 1, "getlog", &cb->getlog);

		init.platformData.ndt = getfield(L, "ndt");
		init.platformData.nwh = getfield(L, "nwh");
		init.platformData.context = getfield(L, "context");
		init.platformData.backBuffer = getfield(L, "backBuffer");
		init.platformData.backBufferDS = getfield(L, "backBufferDS");

		//if (init.debug) {
			luabgfx_getalloc(&init.allocator);
		//}
	}

	if (!BGFX(init)(&init)) {
		return luaL_error(L, "bgfx init failed");
	}
	return 0;
}

static void
push_renderer_type(lua_State *L, bgfx_renderer_type_t t) {
	const char *st;
#define RENDERER_TYPE(x) case BGFX_RENDERER_TYPE_##x : st = #x; break
	switch(t) {
		RENDERER_TYPE(NOOP);
		RENDERER_TYPE(DIRECT3D9);
		RENDERER_TYPE(DIRECT3D11);
		RENDERER_TYPE(DIRECT3D12);
		RENDERER_TYPE(GNM);
		RENDERER_TYPE(METAL);
		RENDERER_TYPE(OPENGLES);
		RENDERER_TYPE(OPENGL);
		RENDERER_TYPE(VULKAN);
		default: {
			luaL_error(L, "Unknown renderer type %d", t);
			return;
		}
	}
	lua_pushstring(L, st);
}

static void
push_supported(lua_State *L, uint64_t supported) {
#define CAPSNAME(v) { BGFX_CAPS_##v, #v },
	struct {
		uint64_t caps;
		const char *name;
	} flags[] = {
		CAPSNAME(ALPHA_TO_COVERAGE)     // Alpha to coverage is supported.
		CAPSNAME(BLEND_INDEPENDENT)     // Blend independent is supported.
		CAPSNAME(COMPUTE)               // Compute shaders are supported.
		CAPSNAME(CONSERVATIVE_RASTER)   // Conservative rasterization is supported.
		CAPSNAME(DRAW_INDIRECT)         // Draw indirect is supported.
		CAPSNAME(FRAGMENT_DEPTH)        // Fragment depth is accessible in fragment shader.
		CAPSNAME(FRAGMENT_ORDERING)     // Fragment ordering is available in fragment shader.
		CAPSNAME(GRAPHICS_DEBUGGER)     // Graphics debugger is present.
		CAPSNAME(HDR10)                 // HDR10 rendering is supported.
		CAPSNAME(HIDPI)                 // HiDPI rendering is supported.
		CAPSNAME(INDEX32)               // 32-bit indices are supported.
		CAPSNAME(INSTANCING)            // Instancing is supported.
		CAPSNAME(OCCLUSION_QUERY)       // Occlusion query is supported.
		CAPSNAME(RENDERER_MULTITHREADED)// Renderer is on separate thread.
		CAPSNAME(SWAP_CHAIN)            // Multiple windows are supported.
		CAPSNAME(TEXTURE_2D_ARRAY)      // 2D texture array is supported.
		CAPSNAME(TEXTURE_3D)            // 3D textures are supported.
		CAPSNAME(TEXTURE_BLIT)          // Texture blit is supported.
		CAPSNAME(TEXTURE_COMPARE_ALL)   // All texture compare modes are supported.
		CAPSNAME(TEXTURE_COMPARE_LEQUAL)// Texture compare less equal mode is supported.
		CAPSNAME(TEXTURE_CUBE_ARRAY)    // Cubemap texture array is supported.
		CAPSNAME(TEXTURE_DIRECT_ACCESS) // CPU direct access to GPU texture memory.
		CAPSNAME(TEXTURE_READ_BACK)     // Read-back texture is supported.
		CAPSNAME(VERTEX_ATTRIB_HALF)    // Vertex attribute half-float is supported.
		CAPSNAME(VERTEX_ATTRIB_UINT10)  // Vertex attribute 10_10_10_2 is supported.
	};
	int n = sizeof(flags) / sizeof(flags[0]);
	lua_createtable(L, 0, n);
	int i;
	for (i=0;i<n;i++) {
		lua_pushboolean(L, supported & flags[i].caps);
		lua_setfield(L, -2, flags[i].name);
	}
}

static void
push_vendor_id(lua_State *L, uint16_t vid) {
	if (vid == BGFX_PCI_ID_SOFTWARE_RASTERIZER) {
		lua_pushstring(L, "SOFTWARE_RASTERIZER");
	} else if (vid == BGFX_PCI_ID_AMD) {
		lua_pushstring(L, "AMD");
	} else if (vid == BGFX_PCI_ID_INTEL) {
		lua_pushstring(L, "INTEL");
	} else if (vid == BGFX_PCI_ID_NVIDIA) {
		lua_pushstring(L, "NVIDIA");
	} else {
		lua_pushinteger(L, vid);
	}
}

static int
find_texture_format(const uint16_t *formats, int index) {
	int i;
	uint16_t c = formats[index];
	for (i=0;i<index;i++) {
		if (c == formats[i]) {
			return i+1;
		}
	}
	return 0;
}

#define TFNAME(v) #v,

static const char * c_texture_formats[] = {
	TFNAME(BC1)
	TFNAME(BC2)
	TFNAME(BC3)
	TFNAME(BC4)
	TFNAME(BC5)
	TFNAME(BC6H)
	TFNAME(BC7)
	TFNAME(ETC1)
	TFNAME(ETC2)
	TFNAME(ETC2A)
	TFNAME(ETC2A1)
	TFNAME(PTC12)
	TFNAME(PTC14)
	TFNAME(PTC12A)
	TFNAME(PTC14A)
	TFNAME(PTC22)
	TFNAME(PTC24)

	TFNAME(ATC)
	TFNAME(ATCE)
	TFNAME(ATCI)
	TFNAME(ASTC4X4)
	TFNAME(ASTC5X5)
	TFNAME(ASTC6X6)
	TFNAME(ASTC8X5)
	TFNAME(ASTC8X6)
	TFNAME(ASTC10X5)

	TFNAME(UNKNOWN)
	TFNAME(R1)
	TFNAME(A8)
	TFNAME(R8)
	TFNAME(R8I)
	TFNAME(R8U)
	TFNAME(R8S)
	TFNAME(R16)
	TFNAME(R16I)
	TFNAME(R16U)
	TFNAME(R16F)
	TFNAME(R16S)
	TFNAME(R32I)
	TFNAME(R32U)
	TFNAME(R32F)
	TFNAME(RG8)
	TFNAME(RG8I)
	TFNAME(RG8U)
	TFNAME(RG8S)
	TFNAME(RG16)
	TFNAME(RG16I)
	TFNAME(RG16U)
	TFNAME(RG16F)
	TFNAME(RG16S)
	TFNAME(RG32I)
	TFNAME(RG32U)
	TFNAME(RG32F)
	TFNAME(RGB8)
	TFNAME(RGB8I)
	TFNAME(RGB8U)
	TFNAME(RGB8S)
	TFNAME(RGB9E5F)
	TFNAME(BGRA8)
	TFNAME(RGBA8)
	TFNAME(RGBA8I)
	TFNAME(RGBA8U)
	TFNAME(RGBA8S)
	TFNAME(RGBA16)
	TFNAME(RGBA16I)
	TFNAME(RGBA16U)
	TFNAME(RGBA16F)
	TFNAME(RGBA16S)
	TFNAME(RGBA32I)
	TFNAME(RGBA32U)
	TFNAME(RGBA32F)
	TFNAME(R5G6B5)
	TFNAME(RGBA4)
	TFNAME(RGB5A1)
	TFNAME(RGB10A2)
	TFNAME(RG11B10F)
	TFNAME(UNKNOWNDEPTH)
	TFNAME(D16)
	TFNAME(D24)
	TFNAME(D24S8)
	TFNAME(D32)
	TFNAME(D16F)
	TFNAME(D24F)
	TFNAME(D32F)
	TFNAME(D0S8)


};

static void
push_texture_formats(lua_State *L, const uint16_t *formats) {
#define CAPSTF(v) { BGFX_CAPS_FORMAT_TEXTURE_##v, #v },
	struct {
		uint16_t caps;
		const char * name;
	} caps_texture_format[] = {
		CAPSTF(2D)               //Texture format is supported.
		CAPSTF(2D_SRGB)          //Texture as sRGB format is supported.
		CAPSTF(2D_EMULATED)      //Texture format is emulated.
		CAPSTF(3D)               //Texture format is supported.
		CAPSTF(3D_SRGB)          //Texture as sRGB format is supported.
		CAPSTF(3D_EMULATED)      //Texture format is emulated.
		CAPSTF(CUBE)             //Texture format is supported.
		CAPSTF(CUBE_SRGB)        //Texture as sRGB format is supported.
		CAPSTF(CUBE_EMULATED)    //Texture format is emulated.
		CAPSTF(VERTEX)           //Texture format can be used from vertex shader.
		CAPSTF(IMAGE)            //Texture format can be used as image from compute shader.
		CAPSTF(FRAMEBUFFER)      //Texture format can be used as frame buffer.
		CAPSTF(FRAMEBUFFER_MSAA) //Texture format can be used as MSAA frame buffer.
		CAPSTF(MSAA)             //Texture can be sampled as MSAA.
		CAPSTF(MIP_AUTOGEN)      //Texture format supports auto-generated mips.
	};
	lua_createtable(L, 0, BGFX_TEXTURE_FORMAT_COUNT);
	int i,j;
	int ncaps = sizeof(caps_texture_format) / sizeof(caps_texture_format[0]);
	for (i=0;i<BGFX_TEXTURE_FORMAT_COUNT;i++) {
		uint16_t c = formats[i];
		int sameindex = find_texture_format(formats, i);
		if (sameindex) {
			lua_getfield(L, -1, c_texture_formats[sameindex-1]);
		} else {
			lua_newtable(L);
			for (j=0;j<ncaps;j++) {
				if (c & caps_texture_format[j].caps) {
					lua_pushboolean(L, 1);
					lua_setfield(L, -2, caps_texture_format[j].name);
				}
			}
		}
		lua_setfield(L, -2, c_texture_formats[i]);
	}
}

static void
push_limits(lua_State *L, const bgfx_caps_limits_t *lim) {
	lua_createtable(L, 0, 23);
#define PUSH_LIMIT(what) lua_pushinteger(L, lim->what); lua_setfield(L, -2, #what);

	PUSH_LIMIT(maxDrawCalls)
	PUSH_LIMIT(maxBlits)
	PUSH_LIMIT(maxTextureSize)
	PUSH_LIMIT(maxTextureLayers)
	PUSH_LIMIT(maxViews)
	PUSH_LIMIT(maxFrameBuffers)
	PUSH_LIMIT(maxFBAttachments)
	PUSH_LIMIT(maxPrograms)
	PUSH_LIMIT(maxShaders)
	PUSH_LIMIT(maxTextures)
	PUSH_LIMIT(maxTextureSamplers)
	PUSH_LIMIT(maxComputeBindings)
	PUSH_LIMIT(maxVertexLayouts)
	PUSH_LIMIT(maxVertexStreams)
	PUSH_LIMIT(maxIndexBuffers)
	PUSH_LIMIT(maxVertexBuffers)
	PUSH_LIMIT(maxDynamicIndexBuffers)
	PUSH_LIMIT(maxDynamicVertexBuffers)
	PUSH_LIMIT(maxUniforms)
	PUSH_LIMIT(maxOcclusionQueries)
	PUSH_LIMIT(maxEncoders)
	PUSH_LIMIT(transientVbSize)
	PUSH_LIMIT(transientIbSize)
}

static int
lgetCaps(lua_State *L) {
	const bgfx_caps_t * caps = BGFX(get_caps)();
	lua_createtable(L, 0, 9);
	push_renderer_type(L, caps->rendererType);
	lua_setfield(L, -2, "rendererType");
	push_supported(L, caps->supported);
	lua_setfield(L, -2, "supported");
	push_vendor_id(L, caps->vendorId);
	lua_setfield(L, -2, "vendorId");
	lua_pushinteger(L, caps->deviceId);
	lua_setfield(L, -2, "deviceId");
	lua_pushboolean(L, caps->homogeneousDepth);
	lua_setfield(L, -2, "homogeneousDepth");
	lua_pushboolean(L, caps->originBottomLeft);
	lua_setfield(L, -2, "originBottomLeft");
	lua_createtable(L, caps->numGPUs, 0);
	int i;
	for (i=0;i<caps->numGPUs;i++) {
		lua_createtable(L, 0, 2);
		push_vendor_id(L, caps->gpu[i].vendorId);
		lua_setfield(L, -2, "vendorId");
		lua_pushinteger(L, caps->gpu[i].deviceId);
		lua_setfield(L, -2, "deviceId");
		lua_seti(L, -2, i+1);
	}
	lua_setfield(L, -2, "gpu");
	push_texture_formats(L, caps->formats);
	lua_setfield(L, -2, "formats");
	push_limits(L, &caps->limits);
	lua_setfield(L, -2, "limits");
	return 1;
}

static int
lgetStats(lua_State *L) {
	size_t sz;
	const char * what = luaL_checklstring(L, 1, &sz);
	lua_settop(L, 2);
	if (!lua_istable(L, 2)) {
		lua_settop(L, 1);
		lua_createtable(L, 0, sz);
	}
	const bgfx_stats_t * stat = BGFX(get_stats)();
	int i;
#define PUSHSTAT(v) lua_pushinteger(L, stat->v); lua_setfield(L, 2, #v)
	for (i=0;what[i];++i) {
	switch(what[i]) {
	case 's':	// back buffer size
		PUSHSTAT(width);
		PUSHSTAT(height);
		break;
	case 'd': // debug size
		PUSHSTAT(textWidth);
		PUSHSTAT(textHeight);
		break;
	case 'c':	// draw calls
		PUSHSTAT(numDraw);
		PUSHSTAT(numBlit);
		PUSHSTAT(numCompute);
		PUSHSTAT(maxGpuLatency);
		break;
	case 'p':	// numPrims
#define PUSHPRIM(v,name) lua_pushinteger(L, stat->numPrims[v]); lua_setfield(L, 2, name)
		PUSHPRIM(BGFX_TOPOLOGY_TRI_LIST, "numTriList");
		PUSHPRIM(BGFX_TOPOLOGY_TRI_STRIP, "numTriStrip");
		PUSHPRIM(BGFX_TOPOLOGY_LINE_LIST, "numLineList");
		PUSHPRIM(BGFX_TOPOLOGY_LINE_STRIP, "numLineStrip");
		PUSHPRIM(BGFX_TOPOLOGY_POINT_LIST, "numPointList");
		break;
	case 'n':	// numbers
		PUSHSTAT(numDynamicIndexBuffers);
		PUSHSTAT(numDynamicVertexBuffers);
		PUSHSTAT(numFrameBuffers);
		PUSHSTAT(numIndexBuffers);
		PUSHSTAT(numOcclusionQueries);
		PUSHSTAT(numPrograms);
		PUSHSTAT(numShaders);
		PUSHSTAT(numTextures);
		PUSHSTAT(numUniforms);
		PUSHSTAT(numVertexBuffers);
		PUSHSTAT(numVertexLayouts);
	case 'm': // memories
		PUSHSTAT(textureMemoryUsed);
		PUSHSTAT(rtMemoryUsed);
		PUSHSTAT(transientVbUsed);
		PUSHSTAT(transientIbUsed);
		break;
	case 't':	// timers
		PUSHSTAT(waitSubmit); break;
		PUSHSTAT(waitRender); break;
		lua_pushnumber(L, (stat->cpuTimeEnd - stat->cpuTimeBegin)*1000.0/stat->cpuTimerFreq);
		lua_setfield(L, 2, "cpu");
		lua_pushnumber(L, (stat->gpuTimeEnd - stat->gpuTimeBegin)*1000.0/stat->gpuTimerFreq);
		lua_setfield(L, 2, "gpu");
		break;
	case 'v': {	// views
		if (lua_getfield(L, 2, "view") != LUA_TTABLE) {
			lua_pop(L, 1);
			lua_createtable(L, stat->numViews, 0);
		}
		int i;
		int n = lua_rawlen(L, -1);
		if (n > stat->numViews) {
			for (i=n+1;i<=n;i++) {
				lua_pushnil(L);
				lua_seti(L, -2, i);
			}
		}
		double cpums = 1000.0 / stat->cpuTimerFreq;
		double gpums = 1000.0 / stat->gpuTimerFreq;
		for (i=0;i<stat->numViews;++i) {
			if (lua_geti(L, -1, i+1) != LUA_TTABLE) {
				lua_pop(L, 1);
				lua_createtable(L, 0, 4);
			}
			lua_pushstring(L, stat->viewStats[i].name);
			lua_setfield(L, -2, "name");
			lua_pushinteger(L, stat->viewStats[i].view);
			lua_setfield(L, -2, "view");
			lua_pushnumber(L, (stat->viewStats[i].cpuTimeEnd - stat->viewStats[i].cpuTimeBegin) * cpums);
			lua_setfield(L, -2, "cpu");
			lua_pushnumber(L, (stat->viewStats[i].gpuTimeEnd - stat->viewStats[i].gpuTimeBegin) * gpums);
			lua_setfield(L, -2, "gpu");
			lua_pop(L, 1);
		}
		lua_setfield(L, 2, "view");
		break;
	}
	default:
		return luaL_error(L, "Unkown stat format %c", what[i]);
	}}
	return 1;
}

static int
lgetMemory(lua_State *L) {
	int64_t memory = 0;
	luabgfx_info(&memory);
	lua_pushinteger(L, memory);
	return 1;
}

/*
	f        : BGFX_RESET_FULLSCREEN - Not supported yet.
	m2/4/8/x : BGFX_RESET_MSAA_X[2/4/8/16] - Enable 2, 4, 8 or 16 x MSAA.
	v        : BGFX_RESET_VSYNC - Enable V-Sync.
	a        : BGFX_RESET_MAXANISOTROPY - Turn on/off max anisotropy.
	c        : BGFX_RESET_CAPTURE - Begin screen capture.
	u        : BGFX_RESET_FLUSH_AFTER_RENDER - Flush rendering after submitting to GPU.
	i        : BGFX_RESET_FLIP_AFTER_RENDER - This flag specifies where flip occurs. Default behavior is that flip occurs before rendering new frame. This flag only has effect when BGFX_CONFIG_MULTITHREADED=0.
	s        : BGFX_RESET_SRGB_BACKBUFFER - Enable sRGB backbuffer.
	h        : BGFX_RESET_HDR10 - Enable HDR10 rendering.
	p        : BGFX_RESET_HIDPI - Enable HiDPI rendering.
	d        : BGFX_RESET_DEPTH_CLAMP - Enable depth clamp.
	z        : BGFX_RESET_SUSPEND - Suspend rendering.
*/
static int
lreset(lua_State *L) {
	int width = luaL_checkinteger(L, 1);
	int height = luaL_checkinteger(L, 2);
	uint32_t f = reset_flags(L, 3);
	bgfx_texture_format_t fmt = BGFX_TEXTURE_FORMAT_COUNT;
	if (lua_isstring(L, 4)) {
		fmt = texture_format_from_string(L, 4);
	}
	BGFX(reset)(width, height, f, fmt);
	return 0;
}

static int
lshutdown(lua_State *L) {
	BGFX(shutdown)();
	if (lua_getfield(L, LUA_REGISTRYINDEX, "bgfx_cb") == LUA_TUSERDATA) {
//		struct callback *cb = lua_touserdata(L, -1);
//		if (cb->L) {
//			lua_close(cb->L);
//			cb->L = NULL;
//		}
	}

	return 0;
}

/*
	C : BGFX_CLEAR_COLOR
	D : BGFX_CLEAR_DEPTH
	S : BGFX_CLEAR_STENCIL
	0-7 : BGFX_CLEAR_DISCARD_COLOR_*
	d : BGFX_CLEAR_DISCARD_DEPTH
	s : BGFX_CLEAR_DISCARD_STENCIL
 */

static int
clear_flags(lua_State *L, const char *flags) {
	int flag = BGFX_CLEAR_NONE;
	int i;
	for (i=0;flags[i];i++) {
		switch(flags[i]) {
#define CLEAR_FLAG(x) flag |= BGFX_CLEAR_##x; break
		case 'C' : CLEAR_FLAG(COLOR);
		case 'D' : CLEAR_FLAG(DEPTH);
		case 'S' : CLEAR_FLAG(STENCIL);
		case '0' : CLEAR_FLAG(DISCARD_COLOR_0);
		case '1' : CLEAR_FLAG(DISCARD_COLOR_1);
		case '2' : CLEAR_FLAG(DISCARD_COLOR_2);
		case '3' : CLEAR_FLAG(DISCARD_COLOR_3);
		case '4' : CLEAR_FLAG(DISCARD_COLOR_4);
		case '5' : CLEAR_FLAG(DISCARD_COLOR_5);
		case '6' : CLEAR_FLAG(DISCARD_COLOR_6);
		case '7' : CLEAR_FLAG(DISCARD_COLOR_7);
		case 'd' : CLEAR_FLAG(DISCARD_DEPTH);
		case 's' : CLEAR_FLAG(DISCARD_STENCIL);
		default:
			return luaL_error(L, "Invalid clear flag %c", flags[i]);
		}
	}
	return flag;
}

static int
lsetViewClear(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	const char * flags = luaL_checkstring(L, 2);
	uint32_t rgba = luaL_optinteger(L, 3, 0x000000ff);
	float depth = luaL_optnumber(L, 4, 1.0f);
	int stencil = luaL_optinteger(L, 5, 0);
	int flag = clear_flags(L, flags);
	BGFX(set_view_clear)(viewid, flag, rgba, depth, stencil);
	return 0;
}

static int
lsetViewClearMRT(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	const char * flags = luaL_checkstring(L, 2);
	int flag = clear_flags(L, flags);
	float depth = luaL_checknumber(L, 3);
	int stencil = luaL_checkinteger(L, 4);
	uint8_t c[8];
	memset(c, UINT8_MAX, 8);
	int n = lua_gettop(L) - 4;
	int i;
	for (i=0;i<n;i++) {
		c[i] = (uint8_t)luaL_optinteger(L, 5+i, UINT8_MAX);
	}
	BGFX(set_view_clear_mrt)(viewid, flag, depth, stencil,
		c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7]);
	return 0;
}

static int
ltouch(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	BGFX(touch)(viewid);
	return 0;
}

static int
lframe(lua_State *L) {
	int capture = lua_toboolean(L, 1);
	int frame = BGFX(frame)(capture);
	lua_pushinteger(L, frame);
	return 1;
}

static int
lsetDebug(lua_State *L) {
	const char *flags = luaL_checkstring(L, 1);
	int flag = BGFX_DEBUG_NONE;
	int i;
	for (i=0;flags[i];i++) {
		switch(flags[i]) {
#define DEBUG_FLAG(x) flag |= BGFX_DEBUG_##x; break
		case 'I' : DEBUG_FLAG(IFH);
		case 'W' : DEBUG_FLAG(WIREFRAME);
		case 'S' : DEBUG_FLAG(STATS);
		case 'T' : DEBUG_FLAG(TEXT);
		case 'P' : DEBUG_FLAG(PROFILER);
		default:
			return luaL_error(L, "Invalid debug flag %c", flags[i]);
		}
	}
	BGFX(set_debug)(flag);
	return 0;
}

static int
lcreateShader(lua_State *L) {
	size_t sz;
	const char *s = luaL_checklstring(L, 1, &sz);
	const bgfx_memory_t * m = BGFX(copy)(s, sz);
	bgfx_shader_handle_t handle = BGFX(create_shader)(m);
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create shader failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(SHADER, handle));
	return 1;
}

static int
ldestroy(lua_State *L) {
	if (lua_isnoneornil(L, 1))
		return 0;
	int idx = luaL_checkinteger(L, 1);
	int type = idx >> 16;
	int id = idx & 0xffff;
	switch(type) {
	case BGFX_HANDLE_PROGRAM: {
		bgfx_program_handle_t  handle = { id };
		BGFX(destroy_program)(handle);
		break;
	}
	case BGFX_HANDLE_SHADER: {
		bgfx_shader_handle_t handle = { id };
		BGFX(destroy_shader)(handle);
		break;
	}
	case BGFX_HANDLE_VERTEX_BUFFER: {
		bgfx_vertex_buffer_handle_t handle = { id };
		BGFX(destroy_vertex_buffer)(handle);
		break;
	}
	case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER: {
		bgfx_dynamic_vertex_buffer_handle_t handle = { id };
		BGFX(destroy_dynamic_vertex_buffer)(handle);
		break;
	}
	case BGFX_HANDLE_INDEX_BUFFER: {
		bgfx_index_buffer_handle_t handle = { id };
		BGFX(destroy_index_buffer)(handle);
		break;
	}
	case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER_32:
	case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER : {
		bgfx_dynamic_index_buffer_handle_t handle = { id };
		BGFX(destroy_dynamic_index_buffer)(handle);
		break;
	}
	case BGFX_HANDLE_UNIFORM : {
		bgfx_uniform_handle_t handle = { id };
		BGFX(destroy_uniform)(handle);
		break;
	}
	case BGFX_HANDLE_TEXTURE : {
		bgfx_texture_handle_t handle = { id };
		BGFX(destroy_texture)(handle);
		break;
	}
	case BGFX_HANDLE_FRAME_BUFFER: {
		bgfx_frame_buffer_handle_t handle = { id };
		BGFX(destroy_frame_buffer)(handle);
		break;
	}
	case BGFX_HANDLE_OCCLUSION_QUERY: {
		bgfx_occlusion_query_handle_t handle = { id };
		BGFX(destroy_occlusion_query)(handle);
		break;
	}
	case BGFX_HANDLE_INDIRECT_BUFFER: {
		bgfx_indirect_buffer_handle_t handle = { id };
		BGFX(destroy_indirect_buffer)(handle);
		break;
	}
	default:
		return luaL_error(L, "Invalid handle (id=%x)", idx);
	}
	return 0;
}

static int
lcreateProgram(lua_State *L) {
	uint16_t vs = BGFX_LUAHANDLE_ID(SHADER, luaL_checkinteger(L, 1));
	uint16_t fs = UINT16_MAX;
	int t = lua_type(L, 2);
	int d = 0;
	int compute = 0;
	switch (t) {
	case LUA_TBOOLEAN:
		d = lua_toboolean(L, 2);
		compute = 1;
		break;
	case LUA_TNONE:
		compute = 1;
		break;
	case LUA_TNUMBER:
		fs = BGFX_LUAHANDLE_ID(SHADER, luaL_checkinteger(L, 2));
		// FALLTHROUGH
	case LUA_TNIL:
		d = lua_toboolean(L, 3);
		break;
	};
	bgfx_program_handle_t ph;
	if (compute) {
		bgfx_shader_handle_t csh = { vs };
		ph = BGFX(create_compute_program)(csh, d);
	} else {
		bgfx_shader_handle_t vsh = { vs };
		bgfx_shader_handle_t fsh = { fs };
		ph = BGFX(create_program)(vsh, fsh, d);
	}
	if (!BGFX_HANDLE_IS_VALID(ph)) {
		return luaL_error(L, "create program failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(PROGRAM, ph));
	return 1;
}

/*
	b BGFX_DISCARD_BINDINGS
	i BGFX_DISCARD_INDEX_BUFFER
	d BGFX_DISCARD_INSTANCE_DATA
	s BGFX_DISCARD_STATE
	t BGFX_DISCARD_TRANSFORM
	v BGFX_DISCARD_VERTEX_STREAMS
*/
static uint8_t
discard_flags(lua_State *L, int index) {
	if (lua_isnoneornil(L, index)) {
		return BGFX_DISCARD_ALL;
	}
	const char * flags_string = luaL_checkstring(L, index);
	uint8_t flags = 0;
	int i;
	for (i=0;flags_string[i];i++) {
		switch(flags_string[i]) {
		case 'b':
			flags |= BGFX_DISCARD_BINDINGS;
			break;
		case 'i':
			flags |= BGFX_DISCARD_INDEX_BUFFER;
			break;
		case 'd':
			flags |= BGFX_DISCARD_INSTANCE_DATA;
			break;
		case 's':
			flags |= BGFX_DISCARD_STATE;
			break;
		case 't':
			flags |= BGFX_DISCARD_TRANSFORM;
			break;
		case 'v':
			flags |= BGFX_DISCARD_VERTEX_STREAMS;
			break;
		case 'a':
			flags |= BGFX_DISCARD_ALL;
		default:
			luaL_error(L, "Invalid discard string %s", flags_string);
		}
	}
	return flags;
}

static int
lsubmit(lua_State *L) {
	bgfx_view_id_t id = luaL_checkinteger(L, 1);
	uint16_t progid = BGFX_LUAHANDLE_ID(PROGRAM, luaL_checkinteger(L, 2));
	uint32_t depth = luaL_optinteger(L, 3, 0);
	uint8_t flags = discard_flags(L, 4);
	bgfx_program_handle_t ph = { progid };
	BGFX(submit)(id, ph, depth, flags);
	return 0;
}

static int
ldiscard(lua_State *L) {
	uint8_t flags = discard_flags(L, 1);
	BGFX(discard)(flags);
	return 0;
}

#define CASE(v) (strcmp(what,#v) == 0)

static uint64_t
get_equation(lua_State *L, const char *e) {
	char what[4] = { e[0], e[1], e[2], 0 };
	if CASE(ADD) return BGFX_STATE_BLEND_EQUATION_ADD;
	if CASE(SUB) return BGFX_STATE_BLEND_EQUATION_SUB;
	if CASE(REV) return BGFX_STATE_BLEND_EQUATION_REVSUB;
	if CASE(MIN) return BGFX_STATE_BLEND_EQUATION_MIN;
	if CASE(MAX) return BGFX_STATE_BLEND_EQUATION_MAX;
	return luaL_error(L, "Invalid BLEND EQUATION mode : %s", what);
}

// 0 : ZERO
// 1 : ONE
// s : SRC_COLOR
// S : INV_SRC_COLOR
// a : SRC_ALPHA
// A : INV_SRC_ALPHA
// b : DST_ALPHA
// B : INV_DST_ALPHA
// d : DST_COLOR
// D : INV_DST_COLOR
// t : SRC_ALPHA_SAT
// f : FACTOR
// F : INV_FACTOR
static uint64_t
get_blend_func(lua_State *L, char t) {
	switch (t) {
	case '0' : return BGFX_STATE_BLEND_ZERO;
	case '1' : return BGFX_STATE_BLEND_ONE;
	case 's' : return BGFX_STATE_BLEND_SRC_COLOR;
	case 'S' : return BGFX_STATE_BLEND_INV_SRC_COLOR;
	case 'a' : return BGFX_STATE_BLEND_SRC_ALPHA;
	case 'A' : return BGFX_STATE_BLEND_INV_SRC_ALPHA;
	case 'b' : return BGFX_STATE_BLEND_DST_ALPHA;
	case 'B' : return BGFX_STATE_BLEND_INV_DST_ALPHA;
	case 'd' : return BGFX_STATE_BLEND_DST_COLOR;
	case 'D' : return BGFX_STATE_BLEND_INV_DST_COLOR;
	case 't' : return BGFX_STATE_BLEND_SRC_ALPHA_SAT;
	case 'f' : return BGFX_STATE_BLEND_FACTOR;
	case 'F' : return BGFX_STATE_BLEND_INV_FACTOR;
	}
	return luaL_error(L, "Invalid BLEND FUNC type : %c", t);
}

static int
combine_state(lua_State *L, uint64_t *state) {
	if (lua_type(L, -2) != LUA_TSTRING) {
		luaL_error(L, "state key must be string, it's %s", lua_typename(L, lua_type(L, -2)));
	}
	const char * what = lua_tostring(L, -2);
	if CASE(PT) {
		*state &= ~BGFX_STATE_PT_MASK;
		const char * what = luaL_checkstring(L, -1);
		if CASE(TRISTRIP) *state |= BGFX_STATE_PT_TRISTRIP;
		else if CASE(LINES) *state |= BGFX_STATE_PT_LINES;
		else if CASE(POINTS) *state |= BGFX_STATE_PT_POINTS;
		else if CASE(LINESTRIP) *state |= BGFX_STATE_PT_LINESTRIP;
		else luaL_error(L, "Invalid PT mode : %s", what);
	} 
	else if CASE(BLEND) {
		*state &= ~BGFX_STATE_BLEND_MASK;
		const char * what = luaL_checkstring(L, -1);
		if CASE(ADD) *state |= BGFX_STATE_BLEND_ADD;
		else if CASE(ALPHA) *state |= BGFX_STATE_BLEND_ALPHA;
		else if CASE(DARKEN) *state |= BGFX_STATE_BLEND_DARKEN;
		else if CASE(LIGHTEN) *state |= BGFX_STATE_BLEND_LIGHTEN;
		else if CASE(MULTIPLY) *state |= BGFX_STATE_BLEND_MULTIPLY;
		else if CASE(NORMAL) *state |= BGFX_STATE_BLEND_NORMAL;
		else if CASE(SCREEN) *state |= BGFX_STATE_BLEND_SCREEN;
		else if CASE(LINEAR_BURN) *state |= BGFX_STATE_BLEND_LINEAR_BURN;
		else luaL_error(L, "Invalid BLEND mode : %s", what);
	}
	else if CASE(BLEND_FUNC) {
		*state &= ~BGFX_STATE_BLEND_MASK;
		size_t sz;
		const char *func = luaL_checklstring(L, -1, &sz);
		if (sz == 2) {
			uint64_t c1 = get_blend_func(L, func[0]);
			uint64_t c2 = get_blend_func(L, func[1]);
			*state |= BGFX_STATE_BLEND_FUNC(c1,c2);
		} else if (sz == 4) {
			uint64_t c1 = get_blend_func(L, func[0]);
			uint64_t c2 = get_blend_func(L, func[1]);
			uint64_t c3 = get_blend_func(L, func[2]);
			uint64_t c4 = get_blend_func(L, func[3]);
			*state |= BGFX_STATE_BLEND_FUNC_SEPARATE(c1,c2,c3,c4);
		} else {
			luaL_error(L, "Invalid BLEND FUNC : %s", func);
		}
	}
	else if CASE(BLEND_EQUATION) {
		*state &= ~BGFX_STATE_BLEND_EQUATION_MASK;
		size_t sz;
		const char * eq = luaL_checklstring(L, -1, &sz);
		if (sz == 3) {
			*state |= BGFX_STATE_BLEND_EQUATION(get_equation(L, eq));
		} else if (sz == 6) {
			*state |= BGFX_STATE_BLEND_EQUATION_SEPARATE(get_equation(L, eq), get_equation(L, eq+3));
		} else {
			luaL_error(L, "Invalid BLEND EQUATION mode : %s", eq);
		}
	}
	else if CASE(BLEND_ENABLE) {
		const char *be = luaL_checkstring(L, -1);
		int i;
		for (i=0;be[i];i++) {
			switch(be[i]) {
			case 'i':
				*state |= BGFX_STATE_BLEND_INDEPENDENT;
				break;
			case 'a':
				*state |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
				break;
			default:
				luaL_error(L, "Invalid BLEND ENABLE mode %s", be);
			}
		}
	}
	else if CASE(ALPHA_REF) {
		int ref = luaL_checkinteger(L, -1);
		*state &= ~BGFX_STATE_ALPHA_REF_MASK;
		*state |= BGFX_STATE_ALPHA_REF(ref);
	} else if CASE(POINT_SIZE) {
		int size = luaL_checkinteger(L, -1);
		*state &= ~BGFX_STATE_POINT_SIZE_MASK;
		*state |= BGFX_STATE_POINT_SIZE(size);
	} else if CASE(MSAA) {
		if (lua_toboolean(L, -1))
			*state |= BGFX_STATE_MSAA;
		else
			*state &= ~BGFX_STATE_MSAA;
	} else if CASE(LINEAA) {
		if (lua_toboolean(L, -1))
			*state |= BGFX_STATE_LINEAA;
		else
			*state &= ~BGFX_STATE_LINEAA;
	} else if CASE(CONSERVATIVE_RASTER) {
		if (lua_toboolean(L, -1))
			*state |= BGFX_STATE_CONSERVATIVE_RASTER;
		else
			*state &= ~BGFX_STATE_CONSERVATIVE_RASTER;
	} else if CASE (FRONT_CCW) {
		if (lua_toboolean(L, -1))
			*state |= BGFX_STATE_FRONT_CCW;
		else
			*state &= ~BGFX_STATE_FRONT_CCW;
	} else if CASE(WRITE_MASK) {
		*state &= ~BGFX_STATE_WRITE_MASK;
		const char * mask = luaL_checkstring(L, -1);
		int i;
		for (i=0;mask[i];i++) {
			switch (mask[i]) {
			case 'R':
				*state |= BGFX_STATE_WRITE_R;
				break;
			case 'G':
				*state |= BGFX_STATE_WRITE_G;
				break;
			case 'B':
				*state |= BGFX_STATE_WRITE_B;
				break;
			case 'A':
				*state |= BGFX_STATE_WRITE_A;
				break;
			case 'Z':
				*state |= BGFX_STATE_WRITE_Z;
				break;
			default:
				return luaL_error(L, "Invalid WRITE_MASK %s", mask);
			}
		}
	} else if CASE(DEPTH_TEST) {
		*state &= ~BGFX_STATE_DEPTH_TEST_MASK;
		const char * what = luaL_checkstring(L, -1);
		if CASE(NEVER) *state |= BGFX_STATE_DEPTH_TEST_NEVER;
		else if CASE(ALWAYS) *state |= BGFX_STATE_DEPTH_TEST_ALWAYS;
		else if CASE(LEQUAL) *state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
		else if CASE(EQUAL) *state |= BGFX_STATE_DEPTH_TEST_EQUAL;
		else if CASE(GEQUAL) *state |= BGFX_STATE_DEPTH_TEST_GEQUAL;
		else if CASE(GREATER) *state |= BGFX_STATE_DEPTH_TEST_GREATER;
		else if CASE(NOTEQUAL) *state |= BGFX_STATE_DEPTH_TEST_NOTEQUAL;
		else if CASE(LESS) *state |= BGFX_STATE_DEPTH_TEST_LESS;
		else if CASE(NONE) ;
		else luaL_error(L, "Invalid DEPTH_TEST mode : %s", what);
	} else if CASE(CULL) {
		*state &= ~BGFX_STATE_CULL_MASK;
		const char * what = luaL_checkstring(L, -1);
		if CASE(CCW) *state |= BGFX_STATE_CULL_CCW;
		else if CASE(CW) *state |= BGFX_STATE_CULL_CW;
		else if CASE(NONE) ;
		else luaL_error(L, "Invalid CULL mode : %s", what);
	} else if CASE(BLEND_FACTOR) {
		return -1;
	}
	else if CASE(BLEND_FUNC_RT1) {
		return 1;
	}
	else if CASE(BLEND_FUNC_RT2) {
		return 2;
	}
	else if CASE(BLEND_FUNC_RT3) {
		return 3;
	}
	return 0;
}

static inline void
byte2hex(uint8_t c, uint8_t *t) {
	static char *hex = "0123456789ABCDEF";
	t[0] = hex[c>>4];
	t[1] = hex[c&0xf];
}

static int inline
hex2n(lua_State *L, char c) {
	if (c>='0' && c<='9')
		return c-'0';
	else if (c>='A' && c<='F')
		return c-'A' + 10;
	else if (c>='a' && c<='f')
		return c-'a' + 10;
	return luaL_error(L, "Invalid state %c", c);
}

static inline void
get_state(lua_State *L, int idx, uint64_t *pstate, uint32_t *prgba) {
	size_t sz;
	const uint8_t * data = (const uint8_t *)luaL_checklstring(L, idx, &sz);
	if (sz != 16 && sz != 24) {
		luaL_error(L, "Invalid state length %d", sz);
	}
	uint64_t state = 0;
	uint32_t rgba = 0;
	int i;
	for (i=0;i<15;i++) {
		state |= hex2n(L,data[i]);
		state <<= 4;
	}
	state |= hex2n(L,data[15]);
	if (sz == 24) {
		for (i=0;i<7;i++) {
			rgba |= hex2n(L,data[16+i]);
			rgba <<= 4;
		}
		rgba |= hex2n(L,data[23]);
	}
	*pstate = state;
	*prgba = rgba;
}

static int
lmakeState(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
/*#define BGFX_STATE_DEFAULT (0          \
			| BGFX_STATE_WRITE_RGB       \
			| BGFX_STATE_WRITE_A         \
			| BGFX_STATE_DEPTH_TEST_LESS \
			| BGFX_STATE_WRITE_Z         \
			| BGFX_STATE_CULL_CW         \
			| BGFX_STATE_MSAA            \
			)
*/
	uint64_t state = 0;
	uint32_t rgba = 0;
	int t = lua_type(L, 2);
	switch(t) {
	case LUA_TSTRING:
		get_state(L, 2, &state, &rgba);
		break;
	case LUA_TNIL:
		state = BGFX_STATE_DEFAULT;
		break;
	}
	lua_pushnil(L);
	int blend_factor = 0;
	while (lua_next(L, 1) != 0) {
		int rt = combine_state(L, &state);
		if (rt) {
			blend_factor = 1;
			if (rt < 0) {
				rgba = luaL_checkinteger(L, -1);
			} else {
				size_t sz;
				const char *func = luaL_checklstring(L, -1, &sz);
				if (sz == 2) {
					uint64_t c1 = get_blend_func(L, func[0]);
					uint64_t c2 = get_blend_func(L, func[1]);
					switch (rt) {
					case 1:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_1(c1,c2);
						break;
					case 2:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_2(c1,c2);
						break;
					case 3:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_3(c1,c2);
						break;
					}
				} else if (sz == 5) {
					uint64_t c1 = get_blend_func(L, func[0]);
					uint64_t c2 = get_blend_func(L, func[1]);
					uint64_t eq = get_equation(L, func+2);
					switch (rt) {
					case 1:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_1E(c1,c2,eq);
						break;
					case 2:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_1E(c1,c2,eq);
						break;
					case 3:
						rgba |= BGFX_STATE_BLEND_FUNC_RT_1E(c1,c2,eq);
						break;
					}
				}
			}
		}
		lua_pop(L, 1);
	}
	uint8_t temp[24];
	int i;
	for (i=0;i<8;i++) {
		byte2hex((state >> ((7-i) * 8)) & 0xff, &temp[i*2]);
	}
	if (blend_factor) {
		for (i=0;i<4;i++) {
			byte2hex( (rgba >> ((3-i) * 8)) & 0xff, &temp[16+i*2]);
		}
		lua_pushlstring(L, (const char *)temp, 24);
	} else {
		lua_pushlstring(L, (const char *)temp, 16);
	}
	return 1;
}

static int
lsetState(lua_State *L) {
	if (lua_isnoneornil(L, 1)) {
		BGFX(set_state)(BGFX_STATE_DEFAULT, 0);
	} else {
		uint64_t state;
		uint32_t rgba;
		get_state(L, 1, &state, &rgba);
		BGFX(set_state)(state, rgba);
	}
	return 0;
}

static void
parse_depth_test(lua_State *L, uint64_t state) {
	const char * value = NULL;
	switch (state & BGFX_STATE_DEPTH_TEST_MASK) {
		case BGFX_STATE_DEPTH_TEST_LESS    : value = "LESS"; break;
		case BGFX_STATE_DEPTH_TEST_LEQUAL  : value = "LEQUAL"; break;
		case BGFX_STATE_DEPTH_TEST_EQUAL   : value = "EQUAL"; break;
		case BGFX_STATE_DEPTH_TEST_GEQUAL  : value = "GEQUAL"; break;
		case BGFX_STATE_DEPTH_TEST_GREATER : value = "GREATER"; break;
		case BGFX_STATE_DEPTH_TEST_NOTEQUAL: value = "NOTEQUAL"; break;
		case BGFX_STATE_DEPTH_TEST_NEVER   : value = "NEVER"; break;
		case BGFX_STATE_DEPTH_TEST_ALWAYS  : value = "ALWAYS"; break;
	}
	if (value) {
		lua_pushstring(L, value);
		lua_setfield(L, -2, "DEPTH_TEST");
	}
}

static void
parse_cull(lua_State *L, uint64_t state) {
	const char * value = NULL;
	switch((state & BGFX_STATE_CULL_MASK)) {
		case BGFX_STATE_CULL_CW  : value = "CW"; break;
		case BGFX_STATE_CULL_CCW : value = "CCW"; break;
	}
	if (value) {
		lua_pushstring(L, value);
		lua_setfield(L, -2, "CULL");
	}
}

static void
parse_write(lua_State *L, uint64_t state) {
	char write[6];
	int idx = 0;
	if (state & BGFX_STATE_WRITE_R) {
		write[idx++] = 'R';
	}
	if (state & BGFX_STATE_WRITE_G) {
		write[idx++] = 'G';
	}
	if (state & BGFX_STATE_WRITE_B) {
		write[idx++] = 'B';
	}
	if (state & BGFX_STATE_WRITE_A) {
		write[idx++] = 'A';
	}
	if (state & BGFX_STATE_WRITE_Z) {
		write[idx++] = 'Z';
	}
	write[idx++] = 0;
	if (idx > 1) {
		lua_pushstring(L, write);
		lua_setfield(L, -2, "WRITE");
	}
}

static void
parse_alpha_ref(lua_State *L, uint64_t state) {
	uint64_t ref = (state & BGFX_STATE_ALPHA_REF_MASK) >> BGFX_STATE_ALPHA_REF_SHIFT;
	if (ref > 0) {
		lua_pushinteger(L, ref);
		lua_setfield(L, -2, "ALPHA_REF");
	}
}

static void
parse_point_size(lua_State *L, uint64_t state) {
	uint64_t psize = (state & BGFX_STATE_POINT_SIZE_MASK) >> BGFX_STATE_POINT_SIZE_SHIFT;
	if (psize > 0) {
		lua_pushinteger(L, psize);
		lua_setfield(L, -2, "POINT_SIZE");
	}
}

static void
parse_pt(lua_State *L, uint64_t state) {
	const char * value = NULL;
	switch (state & BGFX_STATE_PT_MASK) {
		case BGFX_STATE_PT_TRISTRIP : value = "TRISTRIP"; break;
		case BGFX_STATE_PT_LINES    : value = "LINES"; break;
		case BGFX_STATE_PT_POINTS   : value = "POINTS"; break;
		case BGFX_STATE_PT_LINESTRIP: value = "LINESTRIP"; break;
	}
	if (value) {
		lua_pushstring(L, value);
		lua_setfield(L, -2, "PT");
	}
}

static char
parse_blend_func(lua_State *L, uint64_t v) {
	v <<= BGFX_STATE_BLEND_SHIFT;
	switch(v) {
		case BGFX_STATE_BLEND_ZERO         : return '0';
		case BGFX_STATE_BLEND_ONE          : return '1';
		case BGFX_STATE_BLEND_SRC_COLOR    : return 's';
		case BGFX_STATE_BLEND_INV_SRC_COLOR: return 'S';
		case BGFX_STATE_BLEND_SRC_ALPHA    : return 'a';
		case BGFX_STATE_BLEND_INV_SRC_ALPHA: return 'A';
		case BGFX_STATE_BLEND_DST_ALPHA    : return 'b';
		case BGFX_STATE_BLEND_INV_DST_ALPHA: return 'B';
		case BGFX_STATE_BLEND_DST_COLOR    : return 'd';
		case BGFX_STATE_BLEND_INV_DST_COLOR: return 'D';
		case BGFX_STATE_BLEND_SRC_ALPHA_SAT: return 't';
		case BGFX_STATE_BLEND_FACTOR       : return 'f';
		case BGFX_STATE_BLEND_INV_FACTOR   : return 'F';
		default :
			luaL_error(L, "Invalid blend func");
			return 0;
	}
}

static void
parse_equation(lua_State *L, char name[4], uint64_t v) {
	v <<= BGFX_STATE_BLEND_EQUATION_SHIFT;
	switch (v) {
	case BGFX_STATE_BLEND_EQUATION_ADD:
		name[0] = 'A'; name[1] = 'D'; name[2] = 'D'; name[3] = 0;
		break;
	case BGFX_STATE_BLEND_EQUATION_SUB:
		name[0] = 'S'; name[1] = 'U'; name[2] = 'B'; name[3] = 0;
		break;
	case BGFX_STATE_BLEND_EQUATION_REVSUB:
		name[0] = 'R'; name[1] = 'E'; name[2] = 'V'; name[3] = 0;
		break;
	case BGFX_STATE_BLEND_EQUATION_MIN:
		name[0] = 'M'; name[1] = 'I'; name[2] = 'N'; name[3] = 0;
		break;
	case BGFX_STATE_BLEND_EQUATION_MAX:
		name[0] = 'M'; name[1] = 'A'; name[2] = 'X'; name[3] = 0;
		break;
	default:
		luaL_error(L, "Invalid blend equation");
	}
}

static void
parse_blend(lua_State *L, uint64_t state, uint32_t rgba) {
	const char * value = NULL;
	uint64_t blend_state = state & BGFX_STATE_BLEND_MASK;
	int blend_factor = 0;
	switch (blend_state) {
		case BGFX_STATE_BLEND_ADD : value = "ADD"; break;
		case BGFX_STATE_BLEND_ALPHA : value = "ALPHA"; break;
		case BGFX_STATE_BLEND_DARKEN : value = "DARKEN"; break;
		case BGFX_STATE_BLEND_LIGHTEN : value = "LIGHTEN"; break;
		case BGFX_STATE_BLEND_MULTIPLY : value = "MULTIPLY"; break;
		case BGFX_STATE_BLEND_NORMAL : value = "NORMAL"; break;
		case BGFX_STATE_BLEND_SCREEN : value = "SCREEN"; break;
		case BGFX_STATE_BLEND_LINEAR_BURN : value = "LINEAR_BURN"; break;
	}
	if (value) {
		lua_pushstring(L, value);
		lua_setfield(L, -2, "BLEND");
	} else {
		char blend_func[5];
		int i;
		blend_state >>= BGFX_STATE_BLEND_SHIFT;
		if (blend_state) {
			for (i=0;i<4;i++) {
				blend_func[i] = parse_blend_func(L, (blend_state >> (i * 4)) & 0xf);
				if (blend_func[i] == 'f' || blend_func[i] == 'F') {
					blend_factor = 1;
				}
			}
			if (blend_func[0] == blend_func[2] && blend_func[1] == blend_func[3]) {
				// BGFX_STATE_BLEND_FUNC
				blend_func[2] = 0;
			} else {
				// BGFX_STATE_BLEND_FUNC_SEPARATE
				blend_func[4] = 0;
			}
			lua_pushstring(L, blend_func);
			lua_setfield(L, -2, "BLEND_FUNC");
			if (blend_factor) {
				lua_pushinteger(L, rgba);
				lua_setfield(L, -2, "BLEND_FACTOR");
			}
		}
	}
	if (blend_factor == 0) {
		// BLEND_FUNC_RT
		int i;
		static const char * func_rt[3] = {
			"BLEND_FUNC_RT1",
			"BLEND_FUNC_RT2",
			"BLEND_FUNC_RT3",
		};
		for (i=0;i<3;i++) {
			int blend_func_rt = (rgba >> (i*11)) & 0x7ff;
			if (blend_func_rt) {
				char func[7];
				func[0] = parse_blend_func(L, blend_func_rt & 0xf);
				func[1] = parse_blend_func(L, (blend_func_rt >> 4) & 0xf);
				int eq = (blend_func_rt >> 8) & 0x7;
				if (eq) {
					parse_equation(L, func+2, eq);
				} else {
					func[2] = 0;
				}
				lua_pushstring(L, func);
				lua_setfield(L, -2, func_rt[i]);
			}
		}
	}
	int blend_equation = (int)((state & BGFX_STATE_BLEND_EQUATION_MASK) >> BGFX_STATE_BLEND_EQUATION_SHIFT);
	if (blend_equation) {
		char eq[7];
		parse_equation(L, eq, blend_equation & 0x7);
		parse_equation(L, eq+3, (blend_equation >> 3) & 0x7);
		lua_pushstring(L, eq);
		lua_setfield(L, -2, "BLEND_EQUATION");
	}

	char blend_enable[3];
	int idx = 0;
	if (state & BGFX_STATE_BLEND_INDEPENDENT) {
		blend_enable[idx++] = 'i';
	}
	if (state & BGFX_STATE_BLEND_ALPHA_TO_COVERAGE) {
		blend_enable[idx++] = 'a';
	}
	blend_enable[idx++] = 0;
	if (idx > 1) {
		lua_pushstring(L, blend_enable);
		lua_setfield(L, -2, "BLEND_ENABLE");
	}
}

static int
lparseState(lua_State *L) {
	uint64_t state;
	uint32_t rgba;
	get_state(L, 1, &state, &rgba);
	lua_newtable(L);
	parse_depth_test(L, state);
	parse_cull(L, state);
	parse_write(L, state);
	parse_blend(L, state, rgba);
	parse_alpha_ref(L, state);
	parse_pt(L, state);
	parse_point_size(L, state);

	if (state & BGFX_STATE_MSAA) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "MSAA");
	}
	if (state & BGFX_STATE_LINEAA) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "LINEAA");
	}
	if (state & BGFX_STATE_CONSERVATIVE_RASTER) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "CONSERVATIVE_RASTER");
	}
	if (state & BGFX_STATE_FRONT_CCW) {
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "FRONT_CCW");
	}

	return 1;
}

struct AttribNamePairs {
	const char* name;
	bgfx_attrib_t attrib;
};

static struct AttribNamePairs attrib_name_pairs[BGFX_ATTRIB_COUNT] = {
	{ "POSITION", BGFX_ATTRIB_POSITION },
	{ "NORMAL", BGFX_ATTRIB_NORMAL },
	{ "TANGENT", BGFX_ATTRIB_TANGENT },
	{ "BITANGENT", BGFX_ATTRIB_BITANGENT },
	{ "COLOR0", BGFX_ATTRIB_COLOR0 },
	{ "COLOR1", BGFX_ATTRIB_COLOR1 },
	{ "COLOR2", BGFX_ATTRIB_COLOR2 },
	{ "COLOR3", BGFX_ATTRIB_COLOR3 },
	{ "INDICES", BGFX_ATTRIB_INDICES},
	{ "WEIGHT", BGFX_ATTRIB_WEIGHT},
	{ "TEXCOORD0", BGFX_ATTRIB_TEXCOORD0},
	{ "TEXCOORD1", BGFX_ATTRIB_TEXCOORD1},
	{ "TEXCOORD2", BGFX_ATTRIB_TEXCOORD2},
	{ "TEXCOORD3", BGFX_ATTRIB_TEXCOORD3},
	{ "TEXCOORD4", BGFX_ATTRIB_TEXCOORD4},
	{ "TEXCOORD5", BGFX_ATTRIB_TEXCOORD5},
	{ "TEXCOORD6", BGFX_ATTRIB_TEXCOORD6},
	{ "TEXCOORD7", BGFX_ATTRIB_TEXCOORD7},
};

struct AttribTypeNamePairs{
	const char* name;
	bgfx_attrib_type_t type;
};

static struct AttribTypeNamePairs attrib_type_name_pairs[BGFX_ATTRIB_TYPE_COUNT] = {
	{"UINT8", BGFX_ATTRIB_TYPE_UINT8},
	{"UINT10", BGFX_ATTRIB_TYPE_UINT10},
	{"INT16", BGFX_ATTRIB_TYPE_INT16},
	{"HALF", BGFX_ATTRIB_TYPE_HALF},
	{"FLOAT", BGFX_ATTRIB_TYPE_FLOAT},
};

#define ARRAY_COUNT(_ARRAY) sizeof(_ARRAY) / sizeof(_ARRAY[0])

static const bgfx_vertex_layout_t *
get_layout(lua_State *L, int index) {
	const bgfx_vertex_layout_t * layout = (const bgfx_vertex_layout_t *)lua_touserdata(L, index);
	if (layout == NULL) {
		luaL_error(L, "Invalid layout");
	}
	if (lua_rawlen(L, index) != sizeof(bgfx_vertex_layout_t)) {
		luaL_error(L, "Invalid layout");
	}
	return layout;
}

static int
lexportVertexLayout(lua_State *L) {
	const bgfx_vertex_layout_t *decl = get_layout(L, 1);

	lua_newtable(L);
	int num_attrib = 1;
	for (int attrib = BGFX_ATTRIB_POSITION; attrib < BGFX_ATTRIB_COUNT; ++attrib) {
		if (BGFX(vertex_layout_has)(decl, (bgfx_attrib_t)attrib)) {
			lua_newtable(L);

			uint8_t num;
			bool nomalized, as_int;
			bgfx_attrib_type_t attrib_type;
			BGFX(vertex_layout_decode)(decl, (bgfx_attrib_t)attrib, &num, &attrib_type, &nomalized, &as_int);
			assert(attrib_type < BGFX_ATTRIB_TYPE_COUNT);

			lua_pushstring(L, attrib_name_pairs[attrib].name);
			lua_seti(L, -2, 1);

			lua_pushnumber(L, num);
			lua_seti(L, -2, 2);

			lua_pushstring(L, attrib_type_name_pairs[attrib_type].name);
			lua_seti(L, -2, 3);

			lua_pushboolean(L, nomalized ? 1 : 0);
			lua_seti(L, -2, 4);

			lua_pushboolean(L, as_int ? 1 : 0);
			lua_seti(L, -2, 5);

			lua_seti(L, -2, num_attrib++);	// push this table
		}
	}

	luaL_checktype(L, -1, LUA_TTABLE);

	return 1;
}
static inline int
lvertexLayoutStride(lua_State *L) {
	int type = lua_type(L, 1);
	if (type != LUA_TUSERDATA) {
		luaL_error(L, "lvertexLayoutStride : invalid input data");
	}
	const bgfx_vertex_layout_t *decl = get_layout(L, 1);
	lua_pushnumber(L, decl->stride);
	return 1;
}

static inline bgfx_attrib_t find_attrib(const char* what) {
	for (int ii = 0; ii < ARRAY_COUNT(attrib_name_pairs); ++ii) {
		if (strcmp(what, attrib_name_pairs[ii].name) == 0)
			return attrib_name_pairs[ii].attrib;
	}

	return BGFX_ATTRIB_COUNT;
}

static inline bgfx_attrib_type_t find_attrib_type(const char* what) {
	for (int ii = 0; ii < ARRAY_COUNT(attrib_type_name_pairs); ++ii) {
		if (strcmp(what, attrib_type_name_pairs[ii].name) == 0)
			return attrib_type_name_pairs[ii].type;
	}

	return BGFX_ATTRIB_TYPE_COUNT;
}

static void
vertex_layout_add(lua_State *L, bgfx_vertex_layout_t *vd) {
	if (lua_geti(L, -1, 1) != LUA_TSTRING) {
		luaL_error(L, "Invalid attrib enum");
	}
	
	const char * what = lua_tostring(L, -1);
	const bgfx_attrib_t attrib = find_attrib(what);
	if (attrib == BGFX_ATTRIB_COUNT) {
		luaL_error(L, "Invalid arrtib enum : %s", what); return;
	}

	lua_pop(L, 1);

	lua_geti(L, -1, 2);
	int num = lua_tointeger(L, -1);
	if (num < 1 || num > 4) {
		luaL_error(L, "Invalid number of elements : %d", num);
	}
	lua_pop(L, 1);

	if (lua_geti(L, -1, 3) != LUA_TSTRING) {
		luaL_error(L, "Invalid attrib type enum");	
	}

	what = lua_tostring(L, -1);
	
	bgfx_attrib_type_t attrib_type = find_attrib_type(what);
	if (attrib_type == BGFX_ATTRIB_TYPE_COUNT) {
		luaL_error(L, "Invalid arrtib type enum : %s", what); return;
	}
	
	lua_pop(L, 1);

	lua_geti(L, -1, 4);
	int normalized = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_geti(L, -1, 5);
	int asint = lua_toboolean(L, -1);
	lua_pop(L, 1);

	BGFX(vertex_layout_add)(vd, attrib, num, attrib_type, normalized, asint);
}

struct string_reader {
	lua_State *L;
	const char * buffer;
	size_t sz;
	size_t total;
};

static inline uint32_t
read_int(struct string_reader *rd, int n) {
	if (rd->sz < n) {
		return luaL_error(rd->L, "Invalid stream");
	}
	const uint8_t *s = (const uint8_t *)rd->buffer;
	rd->buffer += n;
	rd->sz -= n;
	rd->total += n;
	int i;
	uint32_t v = 0;
	for (i=0;i<n;i++) {
		v |= s[i] << (i * 8);
	}
	return v;
}

static bgfx_attrib_t
idToAttrib(uint16_t id) {
	static struct {
		bgfx_attrib_t attr;
		uint16_t id;
	} s_attribToId[] = {
		// NOTICE: from vetexdecl.cpp
		// Attrib must be in order how it appears in Attrib::Enum! id is
		// unique and should not be changed if new Attribs are added.
		{ BGFX_ATTRIB_POSITION,  0x0001 },
		{ BGFX_ATTRIB_NORMAL,    0x0002 },
		{ BGFX_ATTRIB_TANGENT,   0x0003 },
		{ BGFX_ATTRIB_BITANGENT, 0x0004 },
		{ BGFX_ATTRIB_COLOR0,    0x0005 },
		{ BGFX_ATTRIB_COLOR1,    0x0006 },
		{ BGFX_ATTRIB_COLOR2,    0x0018 },
		{ BGFX_ATTRIB_COLOR3,    0x0019 },
		{ BGFX_ATTRIB_INDICES,   0x000E },
		{ BGFX_ATTRIB_WEIGHT,    0x000F },
		{ BGFX_ATTRIB_TEXCOORD0, 0x0010 },
		{ BGFX_ATTRIB_TEXCOORD1, 0x0011 },
		{ BGFX_ATTRIB_TEXCOORD2, 0x0012 },
		{ BGFX_ATTRIB_TEXCOORD3, 0x0013 },
		{ BGFX_ATTRIB_TEXCOORD4, 0x0014 },
		{ BGFX_ATTRIB_TEXCOORD5, 0x0015 },
		{ BGFX_ATTRIB_TEXCOORD6, 0x0016 },
		{ BGFX_ATTRIB_TEXCOORD7, 0x0017 },
	};

	int i;
	for (i = 0; i < sizeof(s_attribToId) / sizeof(s_attribToId[0]); i++) {
		if (s_attribToId[i].id == id)
			return s_attribToId[i].attr;
	}
	return BGFX_ATTRIB_COUNT;
}

static bgfx_attrib_type_t
idToAttribType(uint16_t id) {
	static struct {
		bgfx_attrib_type_t attr;
		uint16_t id;
	} s_attribTypeToId[] = {
		// NOTICE:
		// AttribType must be in order how it appears in AttribType::Enum!
		// id is unique and should not be changed if new AttribTypes are
		// added.
		{ BGFX_ATTRIB_TYPE_UINT8,  0x0001 },
		{ BGFX_ATTRIB_TYPE_UINT10, 0x0005 },
		{ BGFX_ATTRIB_TYPE_INT16,  0x0002 },
		{ BGFX_ATTRIB_TYPE_HALF,   0x0003 },
		{ BGFX_ATTRIB_TYPE_FLOAT,  0x0004 },
	};
	int i;
	for (i = 0; i < sizeof(s_attribTypeToId) / sizeof(s_attribTypeToId[0]); i++) {
		if (s_attribTypeToId[i].id == id)
			return s_attribTypeToId[i].attr;
	}
	return BGFX_ATTRIB_TYPE_COUNT;
}

static size_t
new_vdecl_from_string(lua_State *L, const char *vdecl, size_t sz) {
	bgfx_vertex_layout_t * vd = lua_newuserdata(L, sizeof(*vd));
	struct string_reader rd = { L, vdecl, sz, 0 };
	uint8_t numAttrs = read_int(&rd, 1);
	uint16_t stride = read_int(&rd, 2);
	BGFX(vertex_layout_begin)(vd, BGFX_RENDERER_TYPE_NOOP);
	int i;
	for (i=0;i<numAttrs;i++) {
		uint16_t offset = read_int(&rd, 2);
		uint16_t attribId = read_int(&rd, 2);
		uint8_t num = read_int(&rd, 1);
		uint16_t attribTypeId = read_int(&rd, 2);
		bool normalized = read_int(&rd, 1);
		bool asInt = read_int(&rd, 1);
		bgfx_attrib_t attr = idToAttrib(attribId);
		bgfx_attrib_type_t type = idToAttribType(attribTypeId);
		if (attr != BGFX_ATTRIB_COUNT && type != BGFX_ATTRIB_TYPE_COUNT) {
			BGFX(vertex_layout_add)(vd, attr, num, type, normalized, asInt);
			vd->offset[attr] = offset;
		}
	}
	BGFX(vertex_layout_end)(vd);
	vd->stride = stride;
	lua_pushinteger(L, stride);
	return rd.total;
}

/*
	{
		{ attrib, num, attribType, normailzed=false, asint },
		skipnum,
		...
	}
 */
static int
lnewVertexLayout(lua_State *L) {
	if (lua_isstring(L, 1)) {
		int offset = luaL_optinteger(L, 2, 1) - 1;
		size_t sz;
		const char * vdecl = luaL_checklstring(L, 1, &sz);
		if (offset >= sz) {
			return luaL_error(L, "Invalid vertex layout");
		}
		size_t s = new_vdecl_from_string(L, vdecl+offset, sz-offset);
		lua_pushinteger(L, s + offset + 1);
		return 3;
	}
	luaL_checktype(L, 1, LUA_TTABLE);
	bgfx_renderer_type_t id = BGFX_RENDERER_TYPE_NOOP;
	if (!lua_isnoneornil(L, 2)) {
		id = renderer_type_id(L, 2);
	}
	bgfx_vertex_layout_t * vd = lua_newuserdata(L, sizeof(*vd));
	BGFX(vertex_layout_begin)(vd, id);
	int i, type;
	for (i=1; (type = lua_geti(L, 1, i)) != LUA_TNIL; i++) {
		switch (type) {
		case LUA_TNUMBER:
			BGFX(vertex_layout_skip)(vd, lua_tointeger(L, -1));
			break;
		case LUA_TTABLE:
			vertex_layout_add(L, vd);
			break;
		default:
			return luaL_error(L, "Invalid vertex layout %s", lua_typename(L, type));
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	BGFX(vertex_layout_end)(vd);
	lua_pushinteger(L, vd->stride);
	return 2;
}

static int
get_stride(lua_State *L, const char *format) {
	int i;
	int stride = 0;
	for (i=0;format[i];i++) {
		switch(format[i]) {
		case 'f':
		case 'd':
			stride += 4;
			break;
		case 's':
			stride += 2;
			break;
		case 'b':
			stride += 1;
			break;
		default:
			luaL_error(L, "invalid layout %c", format[i]);
		}
	}
	return stride;
}

static inline void
copy_layout_data(lua_State*L, const char* layout, int tableidx, uint8_t *addr){
	int startidx = 1;
	int i;
	int type = lua_geti(L, tableidx, startidx);
	while (type != LUA_TNIL) {
		for (i=0;layout[i];i++) {
			if (type != LUA_TNUMBER) {
				luaL_error(L, "buffer data %d type error %s", tableidx, lua_typename(L, type));
			}
			switch (layout[i]) {
			case 'f':
				*(float *)addr = lua_tonumber(L, -1);
				addr += 4;
				break;
			case 'd':
				*(uint32_t *)addr = (uint32_t)lua_tointeger(L, -1);
				addr += 4;
				break;
			case 's':
				*(uint16_t *)addr = (uint16_t)lua_tointeger(L, -1);
				addr += 2;
				break;
			case 'b':
				*(uint8_t *)addr = (uint8_t)lua_tointeger(L, -1);
				addr += 1;
				break;
			}
			lua_pop(L, 1);
			++startidx;
			type = lua_geti(L, tableidx, startidx);
		}
	}
	lua_pop(L, 1);
}

#include <math.h>

static inline float
DOT(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline float *
CROSS(float v[3], const float a[3], const float b[3]) {
	v[0] = a[1] * b[2] - a[2] * b[1];
	v[1] = a[2] * b[0] - a[0] * b[2];
	v[2] = a[0] * b[1] - a[1] * b[0];

	return v;
}

static inline float *
NORMALIZE(float v[3]) {
	float invLen = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	v[0] *= invLen;
	v[1] *= invLen;
	v[2] *= invLen;

	return v;
}
/*
	userdata BGFX_MEMORY
	userdata vertex_layout
	userdata BGFX_MEMORY indices
 */
static int
lcalcTangent(lua_State *L) {
	struct memory *mem = (struct memory *)luaL_checkudata(L, 1, "BGFX_MEMORY");
	if (mem->constant) {
		return luaL_error(L, "It's constant memory object");
	}
	const bgfx_vertex_layout_t *vd = get_layout(L, 2);
	struct memory *indmem = (struct memory *)luaL_checkudata(L, 3, "BGFX_MEMORY");

	void *vertices = mem->data;
	uint32_t numVertices = mem->size / vd->stride;
	uint32_t numIndices;
	if (numVertices > 0x10000) {
		numIndices = indmem->size / sizeof(uint32_t);
	} else {
		numIndices = indmem->size / sizeof(uint16_t);
	}
	float *tangents = (float *)lua_newuserdata(L, 6*numVertices*sizeof(float));
	memset(tangents, 0 , 6*numVertices*sizeof(float));

	struct PosTexcoord {
		float m_x;
		float m_y;
		float m_z;
		float m_pad0;
		float m_u;
		float m_v;
		float m_pad1;
		float m_pad2;
	} v0,v1,v2;

	uint32_t ii,jj,num;
	uint16_t * indices16 = (uint16_t *)indmem->data;
	uint32_t * indices32 = (uint32_t *)indmem->data;
	for (ii = 0, num = numIndices/3; ii < num; ++ii) {
		uint32_t i[3];
		if (numVertices > 0x10000) {
			i[0] = indices32[ii*3];
			i[1] = indices32[ii*3+1];
			i[2] = indices32[ii*3+2];
		} else {
			i[0] = indices16[ii*3];
			i[1] = indices16[ii*3+1];
			i[2] = indices16[ii*3+2];
		}

		BGFX(vertex_unpack)(&v0.m_x, BGFX_ATTRIB_POSITION,  vd, vertices, i[0]);
		BGFX(vertex_unpack)(&v0.m_u, BGFX_ATTRIB_TEXCOORD0, vd, vertices, i[0]);

		BGFX(vertex_unpack)(&v1.m_x, BGFX_ATTRIB_POSITION,  vd, vertices, i[1]);
		BGFX(vertex_unpack)(&v1.m_u, BGFX_ATTRIB_TEXCOORD0, vd, vertices, i[1]);

		BGFX(vertex_unpack)(&v2.m_x, BGFX_ATTRIB_POSITION,  vd, vertices, i[2]);
		BGFX(vertex_unpack)(&v2.m_u, BGFX_ATTRIB_TEXCOORD0, vd, vertices, i[2]);

		const float bax = v1.m_x - v0.m_x;
		const float bay = v1.m_y - v0.m_y;
		const float baz = v1.m_z - v0.m_z;
		const float bau = v1.m_u - v0.m_u;
		const float bav = v1.m_v - v0.m_v;

		const float cax = v2.m_x - v0.m_x;
		const float cay = v2.m_y - v0.m_y;
		const float caz = v2.m_z - v0.m_z;
		const float cau = v2.m_u - v0.m_u;
		const float cav = v2.m_v - v0.m_v;

		const float det = (bau * cav - bav * cau);
		const float invDet = 1.0f / det;

		const float tx = (bax * cav - cax * bav) * invDet;
		const float ty = (bay * cav - cay * bav) * invDet;
		const float tz = (baz * cav - caz * bav) * invDet;

		const float bx = (cax * bau - bax * cau) * invDet;
		const float by = (cay * bau - bay * cau) * invDet;
		const float bz = (caz * bau - baz * cau) * invDet;

		for (jj = 0; jj < 3; ++jj) {
			float* tanu = &tangents[i[jj] * 6];
			float* tanv = &tanu[3];
			tanu[0] += tx;
			tanu[1] += ty;
			tanu[2] += tz;

			tanv[0] += bx;
			tanv[1] += by;
			tanv[2] += bz;
		}
	}

	for (ii = 0; ii < numVertices; ++ii) {
		const float* tanu = &tangents[ii*6];
		const float* tanv = &tangents[ii*6 + 3];

		float normal[4];
		BGFX(vertex_unpack)(normal, BGFX_ATTRIB_NORMAL, vd, vertices, ii);
		float ndt = DOT(normal, tanu);
		float nxt[3];
		CROSS(nxt, normal, tanu);

		float tangent[4];
		tangent[0] = tanu[0] - normal[0] * ndt;
		tangent[1] = tanu[1] - normal[1] * ndt;
		tangent[2] = tanu[2] - normal[2] * ndt;

		NORMALIZE(tangent);

		tangent[3] = DOT(nxt, tanv) < 0.0f ? -1.0f : 1.0f;
		BGFX(vertex_pack)(tangent, true, BGFX_ATTRIB_TANGENT, vd, vertices, ii);
	}

	lua_settop(L, 1);
	return 1;
}

/*
	userdata BGFX_MEMORY
	vertex_layout src
	vertex_layout tar

	return BGFX_MEMORY
 */
static int
lvertexConvert(lua_State *L) {
	struct memory *mem = (struct memory *)luaL_checkudata(L, 1, "BGFX_MEMORY");
	if (mem->constant) {
		luaL_error(L, "It's constant memory object");
	}
	const bgfx_vertex_layout_t *src_vd = get_layout(L, 2);
	const bgfx_vertex_layout_t *tar_vd = get_layout(L, 3);

	int src_stride = src_vd->stride;
	int n = mem->size / src_stride;
	int tar_stride = tar_vd->stride;

	void * tar = newMemory(L, NULL, n * tar_stride);

	BGFX(vertex_convert)(tar_vd, tar, src_vd, mem->data, n);

	return 1;
}

/*
	type 1 :
		string layout
		table number array
	type 2 :
		string data
		integer offset (opt)
		integer size (opt)
	type 3 :
		lightuserdata data
		integer size
		object lifetime (opt) userdata/string/table
	type 4 :
		integer size
 */
static int
lmemoryBuffer(lua_State *L) {
	int t = lua_type(L, 1);
	size_t sz;
	if (t == LUA_TNUMBER) {
		// type 4
		sz = luaL_checkinteger(L, 1);
		newMemory(L, NULL, sz);
		return 1;
	}
	if (t == LUA_TLIGHTUSERDATA) {
		// type 3
		void * data = lua_touserdata(L, 1);
		size_t sz = luaL_checkinteger(L, 2);
		if (lua_gettop(L) == 2) {
			void * buffer = newMemory(L, NULL, sz);
			memcpy(buffer, data, sz);
			return 1;
		}
		lua_settop(L, 3);
		newMemory(L, data, sz);
		return 1;
	}
	const char * str = luaL_checklstring(L, 1, &sz);
	if (lua_gettop(L) == 1) {
		// data only, type 2
		newMemory(L, (void *)str, sz);
		return 1;
	}
	if (lua_type(L, 2) == LUA_TTABLE) {
		// type 1
		int n = lua_rawlen(L, 2) / sz;
		int stride = get_stride(L, str);
		void *data = newMemory(L, NULL, n*stride);
		copy_layout_data(L, str, 2, data);
		return 1;
	}
	// type 2
	int offset = luaL_checkinteger(L, 2);
	if (offset > 0) {
		if (offset > sz) {
			offset = sz;
		}
		--offset;
		str += offset;
		sz -= offset;
	} else if (offset < 0) {
		offset = -offset;
		if (offset > sz) {
			offset = sz;
		}
		offset = sz - offset;
		str += offset;
		sz -= offset;
	}
	sz = luaL_optinteger(L, 3, sz);
	lua_settop(L, 1);
	newMemory(L, (void *)str, sz);
	return 1;
}

static const bgfx_memory_t *
getMemory(lua_State *L, int idx) {
	if (lua_type(L, idx) == LUA_TSTRING) {
		size_t sz;
		const char *data = lua_tolstring(L, idx, &sz);
		lua_pushvalue(L, idx);
		newMemory(L, (void *)data, sz);
		const bgfx_memory_t * mem = bgfxMemory(L, -1);
		lua_pop(L, 1);
		return mem;
	}
	luaL_checkudata(L, idx, "BGFX_MEMORY");
	return bgfxMemory(L, idx);
}

/*
	string / BGFX_MEMORY
	desc
	flags
		r BGFX_BUFFER_COMPUTE_READ 
		w BGFX_BUFFER_COMPUTE_WRITE
		s BGFX_BUFFER_ALLOW_RESIZE ( for dynamic buffer )
		d BGFX_BUFFER_INDEX32 ( for index buffer )
 */
static int
lcreateVertexBuffer(lua_State *L) {
	const bgfx_vertex_layout_t *vd = get_layout(L, 2);

	uint16_t flags = BGFX_BUFFER_NONE;
	if (lua_isstring(L, 3)) {
		const char *f = lua_tostring(L, 3);
		int i;
		for (i=0;f[i];i++) {
			switch(f[i]) {
			case 'r' :
				flags |= BGFX_BUFFER_COMPUTE_READ;
				break;
			case 'w':
				flags |= BGFX_BUFFER_COMPUTE_WRITE;
				break;
			default:
				return luaL_error(L, "Invalid buffer flag %c", f[i]);
			}
		}
	}

	const bgfx_memory_t *mem = getMemory(L, 1);

	bgfx_vertex_buffer_handle_t handle = BGFX(create_vertex_buffer)(mem, vd, flags);
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create vertex buffer failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(VERTEX_BUFFER, handle));
	return 1;
}

static int
lcreateDynamicVertexBuffer(lua_State *L) {
	const bgfx_vertex_layout_t *vd = get_layout(L, 2);
	uint16_t flags = BGFX_BUFFER_NONE;
	if (lua_isstring(L, 3)) {
		const char *f = lua_tostring(L, 3);
		int i;
		for (i=0;f[i];i++) {
			switch(f[i]) {
			case 'r' :
				flags |= BGFX_BUFFER_COMPUTE_READ;
				break;
			case 'w':
				flags |= BGFX_BUFFER_COMPUTE_WRITE;
				break;
			case 'a':
				flags |= BGFX_BUFFER_ALLOW_RESIZE;
				break;
			default:
				return luaL_error(L, "Invalid buffer flag %c", f[i]);
			}
		}
	}

	bgfx_dynamic_vertex_buffer_handle_t handle;
	if (lua_type(L, 1) == LUA_TNUMBER) {
		uint32_t num = luaL_checkinteger(L, 1);
		handle = BGFX(create_dynamic_vertex_buffer)(num, vd, flags);
	} else {
		const bgfx_memory_t *mem = getMemory(L, 1);
		handle = BGFX(create_dynamic_vertex_buffer_mem)(mem, vd, flags);
	}

	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create dynamic vertex buffer failed");
	}

	lua_pushinteger(L, BGFX_LUAHANDLE(DYNAMIC_VERTEX_BUFFER, handle));
	return 1;
}

static uint16_t
index_buffer_flags(lua_State *L, int index) {
	uint16_t flags = BGFX_BUFFER_NONE;
	if (lua_isstring(L, index)) {
		const char *f = lua_tostring(L, index);
		int i;
		for (i=0;f[i];i++) {
			switch(f[i]) {
			case 'r' :
				flags |= BGFX_BUFFER_COMPUTE_READ;
				break;
			case 'w':
				flags |= BGFX_BUFFER_COMPUTE_WRITE;
				break;
			case 'a':
				flags |= BGFX_BUFFER_ALLOW_RESIZE;
				break;
			case 'd':
				flags |= BGFX_BUFFER_INDEX32;
				break;
			default:
				return luaL_error(L, "Invalid buffer flag %c", f[i]);
			}
		}
	}
	return flags;
}

static const bgfx_memory_t *
getIndexBuffer(lua_State *L, int idx, int index32) {
	if (lua_type(L, idx) == LUA_TTABLE) {
		int n = lua_rawlen(L, 1);
		if (index32) {
			void *data = newMemory(L, NULL, n*sizeof(uint32_t));
			copy_layout_data(L, "d", 1, data);
		}
		else {
			void *data = newMemory(L, NULL, n*sizeof(uint16_t));
			copy_layout_data(L, "s", 1, data);
		}
		return bgfxMemory(L, -1);
	} else {
		return getMemory(L, idx);
	}
}

/*
	table data / lightuserdata mem
	boolean 32bit index
 */
static int
lcreateIndexBuffer(lua_State *L) {
	uint16_t flags = index_buffer_flags(L, 2);
	const int index32 = flags & BGFX_BUFFER_INDEX32;
	const bgfx_memory_t *mem = getIndexBuffer(L, 1, index32);
	bgfx_index_buffer_handle_t handle = BGFX(create_index_buffer)(mem, flags);
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create index buffer failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(INDEX_BUFFER, handle));
	return 1;
}

static int
lupdate(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	int idtype = id >> 16;
	int idx = id & 0xffff;
	uint32_t start = luaL_checkinteger(L, 2);
	if (idtype == BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER) {
		const bgfx_memory_t *mem = getMemory(L, 3);
		bgfx_dynamic_vertex_buffer_handle_t handle = { idx };
		BGFX(update_dynamic_vertex_buffer)(handle, start, mem);
		return 0;
	}
	bgfx_dynamic_index_buffer_handle_t handle = { idx };
	const bgfx_memory_t *mem;
	if (idtype == BGFX_HANDLE_DYNAMIC_INDEX_BUFFER) {
		mem = getIndexBuffer(L, 3, 0);
	} else {
		if (idtype != BGFX_HANDLE_DYNAMIC_INDEX_BUFFER_32) {
			return luaL_error(L, "Invalid dynamic index buffer type %d", idtype);
		}
		mem = getIndexBuffer(L, 3, 1);
	}
	BGFX(update_dynamic_index_buffer)(handle, start, mem);
	return 0;
}

static int
lcreateDynamicIndexBuffer(lua_State *L) {
	uint16_t flags = index_buffer_flags(L, 2);

	bgfx_dynamic_index_buffer_handle_t handle;
	if (lua_type(L, 1) == LUA_TNUMBER) {
		uint32_t num = luaL_checkinteger(L, 1);
		handle = BGFX(create_dynamic_index_buffer)(num, flags);
	} else {
		const bgfx_memory_t *mem = getIndexBuffer(L, 1, flags & BGFX_BUFFER_INDEX32);
		handle = BGFX(create_dynamic_index_buffer_mem)(mem, flags);
	}

	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create dynamic index buffer failed");
	}

	if (flags & BGFX_BUFFER_INDEX32) {
		lua_pushinteger(L, BGFX_LUAHANDLE(DYNAMIC_INDEX_BUFFER_32, handle));
	} else {
		lua_pushinteger(L, BGFX_LUAHANDLE(DYNAMIC_INDEX_BUFFER, handle));
	}
	return 1;
}

static int
lsetViewTransform(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	void *view = lua_touserdata(L, 2);	// can be NULL
	if (view == NULL) {
		luaL_checktype(L, 2, LUA_TNIL);
	}
	void *projL = lua_touserdata(L, 3);
	if (projL == NULL) {
		luaL_checktype(L, 3, LUA_TNIL);
	}
	BGFX(set_view_transform)(viewid, view, projL);
	return 0;
}

static int
lsetVertexBuffer(lua_State *L) {
	int stream = 0;
	int start = 0;
	int end = UINT32_MAX;
	int id;
	if (lua_gettop(L) <= 1) {
		if (lua_isnoneornil(L, 1)) {
			// empty
			bgfx_vertex_buffer_handle_t handle = { UINT16_MAX };
			BGFX(set_vertex_buffer)(stream, handle, start, end);
			return 0;
		}
		id = luaL_checkinteger(L, 1);
	} else {
		stream = luaL_checkinteger(L, 1);
		id = luaL_optinteger(L, 2, BGFX_HANDLE_VERTEX_BUFFER | UINT16_MAX);
		start = luaL_optinteger(L, 3, 0);
		end = luaL_optinteger(L, 4, UINT32_MAX);
	}

	int idtype = id >> 16;
	int idx = id & 0xffff;
	if (idtype == BGFX_HANDLE_VERTEX_BUFFER) {
		bgfx_vertex_buffer_handle_t handle = { idx };
		BGFX(set_vertex_buffer)(stream, handle, start, end);
	} else {
		if (idtype != BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER) {
			return luaL_error(L, "Invalid vertex buffer type %d", idtype);
		}
		bgfx_dynamic_vertex_buffer_handle_t handle = { idx };
		BGFX(set_dynamic_vertex_buffer)(stream, handle, start, end);
	}
	return 0;
}

static int
lsetIndexBuffer(lua_State *L) {
	int id = luaL_optinteger(L, 1, BGFX_HANDLE_INDEX_BUFFER | UINT16_MAX);
	int idtype = id >> 16;
	int idx = id & 0xffff;
	int start = luaL_optinteger(L, 2, 0);
	uint32_t end = luaL_optinteger(L, 3, UINT32_MAX); 

	if (idtype == BGFX_HANDLE_INDEX_BUFFER) {
		bgfx_index_buffer_handle_t handle = { idx };
		BGFX(set_index_buffer)(handle, start, end);
	} else {
		if (idtype != BGFX_HANDLE_DYNAMIC_INDEX_BUFFER &&
			idtype != BGFX_HANDLE_DYNAMIC_INDEX_BUFFER_32) {
			return luaL_error(L, "Invalid index buffer type %d", idtype);
		}
		bgfx_dynamic_index_buffer_handle_t handle = { idx };
		BGFX(set_dynamic_index_buffer)(handle, start, end);
	}
	return 0;
}

static int
lsetTransform(lua_State *L) {
	int n = lua_gettop(L);
	if (n == 1) {
		if (!lua_isuserdata(L, 1)) {
			return luaL_error(L, "Need matrix userdata");
		}
		void *mat = lua_touserdata(L, 1);
		int id = BGFX(set_transform)(mat, 1);
		lua_pushinteger(L, id);
		return 1;
	} else if (n < 1) {
		return luaL_error(L, "Need matrix");
	}
	// multiple mats
	bgfx_transform_t trans;
	uint32_t id = BGFX(alloc_transform)(&trans, n);
	if (trans.num > n) {
		return luaL_error(L, "Too many transform");
	}
	int i;
	for (i=0;i<n;i++) {
		if (!lua_isuserdata(L, i+1)) {
			return luaL_error(L, "Need matrix at %d", i+1);
		}
		void *mat = lua_touserdata(L, i+1);
		memcpy(trans.data + 16 * i, mat, 16 * sizeof(float));
	}
	BGFX(set_transform_cached)(id, n);
	lua_pushinteger(L, id);
	return 1;
}

static int
lsetTransformCached(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	int num = luaL_optinteger(L, 2, 1);
	BGFX(set_transform_cached)(id, num);
	return 0;
}

static int
lsetMultiTransforms(lua_State *L){
	int t = lua_type(L, 1);
	int num = luaL_checkinteger(L, 2);
	if (t == LUA_TUSERDATA || t == LUA_TLIGHTUSERDATA) {
		void *mat = lua_touserdata(L, 1);
		int id = BGFX(set_transform)(mat, num);
		lua_pushinteger(L, id);
		return 1;
	}
	return luaL_error(L, "invalid type, need userdata/lightuserdata:%s", lua_typename(L,t));
}

static int
ldbgTextClear(lua_State *L) {
	int attrib = luaL_optinteger(L, 1, 0);
	int s = lua_toboolean(L, 2);
	BGFX(dbg_text_clear)(attrib, s);
	return 0;
}

static int
ldbgTextPrint(lua_State *L) {
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int attrib = luaL_checkinteger(L, 3);
	const char * text = luaL_checkstring(L, 4);
	BGFX(dbg_text_printf)(x,y,attrib,"%s",text);
	return 0;
}

static int
ldbgTextImage(lua_State *L) {
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int w = luaL_checkinteger(L, 3);
	int h = luaL_checkinteger(L, 4);
	const char * image = luaL_checkstring(L, 5);
	int pitch = luaL_optinteger(L, 6, 2 * w);
	BGFX(dbg_text_image)(x,y,w,h,image, pitch);
	return 0;
}

struct ltb {
	bgfx_transient_vertex_buffer_t tvb;
	bgfx_transient_index_buffer_t tib;
	int cap_v;
	int cap_i;
	char format[1];
};

static int
lallocTB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	int max_v = luaL_checkinteger(L, 2);
	int max_i = 0;
	int vd_index = 3;
	if (lua_isinteger(L, 3)) {
		// alloc index
		max_i = lua_tointeger(L, 3);
		vd_index = 4;
	}
	const bgfx_vertex_layout_t *vd = NULL;
	if (max_v) {
		vd = get_layout(L, vd_index);
	}

	if (max_v && max_i) {
		if (!BGFX(alloc_transient_buffers)(&v->tvb, vd, max_v, &v->tib, max_i)) {
			v->cap_v = 0;
			v->cap_i = 0;
			return luaL_error(L, "Alloc transient buffers failed");
		}
	} else {
		if (max_v) {
			BGFX(alloc_transient_vertex_buffer)(&v->tvb, max_v, vd);
		}
		if (max_i) {
			BGFX(alloc_transient_index_buffer)(&v->tib, max_i);
		}
	}
	v->cap_v = max_v;
	v->cap_i = max_i;
	return 0;
}

static int
lsetTVB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	if (v->cap_v == 0) {
		return luaL_error(L, "Need alloc transient vb first");
	}
	int stream = luaL_checkinteger(L, 2);
	int start = luaL_optinteger(L, 3, 0);
	uint32_t end = luaL_optinteger(L, 4, UINT32_MAX); 

	BGFX(set_transient_vertex_buffer)(stream, &v->tvb, start, end);
	v->cap_v = 0;
	return 0;
}

static int
lsetTIB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	if (v->cap_i == 0) {
		return luaL_error(L, "Need alloc transient ib first");
	}
	int start = luaL_optinteger(L, 2, 0);
	uint32_t end = luaL_optinteger(L, 3, UINT32_MAX); 
	BGFX(set_transient_index_buffer)(&v->tib, start, end);
	v->cap_i = 0;
	return 0;
}

static int
lsetTB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	if (v->cap_i) {
		BGFX(set_transient_index_buffer)(&v->tib, 0, v->cap_i);
		v->cap_i = 0;
	}
	if (v->cap_v) {
		BGFX(set_transient_vertex_buffer)(0, &v->tvb, 0, v->cap_v);
		v->cap_v = 0;
	}
	return 0;
}

static int
lpackTVB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	int idx = luaL_checkinteger(L, 2);
	if (idx < 0 || idx >= v->cap_v) {
		return luaL_error(L, "Transient vb index out of range %d/%d", idx, v->cap_v);
	}
	int stride = v->tvb.stride;
	uint8_t * data = v->tvb.data + stride * idx;
	int i;
	int offset = 0;
	for (i=0;v->format[i];i++) {
		if (offset >= stride) {
			return luaL_error(L, "Invalid format %s for stride %d", v->format, stride);
		}
		uint32_t d;
		switch(v->format[i]) {
		case 'f':	// float
			*(float*)(data + offset) = luaL_checknumber(L, 3+i);
			break;
		case 'd':	// dword
			d = (uint32_t)luaL_checkinteger(L, 3+i);
			data[offset] = d & 0xff;
			data[offset+1] = (d >> 8) & 0xff;
			data[offset+2] = (d >> 16) & 0xff;
			data[offset+3] = (d >> 24) & 0xff;
			break;
		case 's':	// skip
			break;
		default:
			return luaL_error(L, "Invalid format %c", v->format[i]);
		}
		offset += 4;
	}
	return 0;
}

static int
lpackTIB(lua_State *L) {
	struct ltb *v = luaL_checkudata(L, 1, "BGFX_TB");
	luaL_checktype(L, 2, LUA_TTABLE);
	uint16_t* indices = (uint16_t*)v->tib.data;
	int i;
	for (i=0;i<v->cap_i;i++) {
		if (lua_geti(L, 2, i+1) != LUA_TNUMBER) {
			luaL_error(L, "Invalid index buffer data %d", i+1);
		}
		int v = lua_tointeger(L, -1);
		lua_pop(L, 1);
		indices[i] = (uint16_t)v;
	}

	return 0;
}

static int
lnewTransientBuffer(lua_State *L) {
	size_t sz;
	const char * format = luaL_checklstring(L, 1, &sz);
	struct ltb * v = lua_newuserdata(L, sizeof(*v) + sz);
	v->cap_v = 0;
	v->cap_i = 0;
	memcpy(v->format, format, sz+1);
	if (luaL_newmetatable(L, "BGFX_TB")) {
		luaL_Reg l[] = {
			{ "alloc", lallocTB },
			{ "setV", lsetTVB },
			{ "setI", lsetTIB },
			{ "set", lsetTB },
			{ "packV", lpackTVB },
			{ "packI", lpackTIB },
			{ NULL, NULL },
		};
		luaL_newlib(L, l);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, lpackTVB);
		lua_setfield(L, -2, "__call");
	}
	lua_setmetatable(L, -2);
	return 1;
}

struct lidb {
	bgfx_instance_data_buffer_t idb;
	int num;
	int stride;
	char format[1];
};

static int
lallocIDB(lua_State *L) {
	struct lidb *v = luaL_checkudata(L, 1, "BGFX_IDB");
	int num = luaL_checkinteger(L, 2);
	BGFX(alloc_instance_data_buffer)(&v->idb, num, v->stride);
	v->num = num;
	return 0;
}

static int
lsetIDB(lua_State *L) {
	struct lidb *v = luaL_checkudata(L, 1, "BGFX_IDB");
	uint32_t num = UINT32_MAX;
	if (lua_isnumber(L, 2)) {
		num = lua_tointeger(L, 2);
		if (num >= (uint32_t)v->num) {
			return luaL_error(L, "Invalid instance data buffer num %d/%d",num, v->num);
		}
	}
	BGFX(set_instance_data_buffer)(&v->idb, 0, num);
	v->num = 0;
	return 0;
}

static int
lpackIDB(lua_State *L) {
	struct lidb *v = luaL_checkudata(L, 1, "BGFX_IDB");
	int idx = luaL_checkinteger(L, 2);
	if (idx < 0 || idx >= v->num) {
		return luaL_error(L, "Instance data buffer index out of range %d/%d", idx, v->num);
	}
	int stride = v->stride;
	float * data = (float *)(v->idb.data + stride * idx);
	int i;
	int lidx = 3;
	int offset = 0;
	float * ud;
	for (i=0;v->format[i];i++) {
		switch(v->format[i]) {
		case 'm':	// matrix 4x4
			ud = lua_touserdata(L, lidx);
			if (ud == NULL) {
				return luaL_error(L, "Missing matrix");
			}
			memcpy(&data[offset], ud, 16 * sizeof(float));
			offset += 16;
			break;
		case 'v':	// vector 4
			if (lua_isnumber(L, lidx)) {
				// 4 floats
				data[offset] = lua_tonumber(L, lidx);
				data[offset+1] = luaL_checknumber(L, lidx+1);
				data[offset+2] = luaL_checknumber(L, lidx+2);
				data[offset+3] = luaL_checknumber(L, lidx+3);
				lidx += 3;
			} else {
				ud = lua_touserdata(L, lidx);
				if (ud == NULL) {
					return luaL_error(L, "Missing vector");
				}
				memcpy(&data[offset], ud, 4 * sizeof(float));
			}
			offset += 4;
			break;
		}
		lidx ++;
	}
	if (lua_gettop(L) != lidx - 1) {
		return luaL_error(L, "too much of data");
	}
	return 0;
}

static int
lformatIDB(lua_State *L) {
	struct lidb *v = (struct lidb *)lua_touserdata(L, 1);
	lua_pushlightuserdata(L, (void *)v->format);
	return 1;
}

static int
lnewInstanceBuffer(lua_State *L) {
	size_t sz;
	const char * format = luaL_checklstring(L, 1, &sz);
	struct lidb * v = lua_newuserdata(L, sizeof(*v) + sz);
	v->num = 0;
	int i;
	int stride = 0;
	for (i=0;format[i];i++) {
		switch(format[i]) {
		case 'm' :
			stride += 4*16;	// matrix 4x4
			break;
		case 'v':
			stride += 4*4;	// vector4
			break;
		default:
			return luaL_error(L, "Invalid instance data buffer format %c", format[i]);
		}
	}
	v->stride = stride;
	memcpy(v->format, format, sz+1);
	luaL_getmetatable(L, "BGFX_IDB");
	lua_setmetatable(L, -2);
	return 1;
}

static int
lgetInstanceBufferMetatable(lua_State *L) {
	luaL_getmetatable(L, "BGFX_IDB");
	return 1;
}

static int
lcreateUniform(lua_State *L) {
	const char * name = luaL_checkstring(L, 1);
	const char * type = luaL_checkstring(L, 2);
	bgfx_uniform_type_t ut;
	switch(type[0]) {
	case 's':
		ut = BGFX_UNIFORM_TYPE_SAMPLER;
		break;
	case 'v':
		if (type[1] != '4') {
			return luaL_error(L, "Invalid Uniform type %s", type);
		}
		ut = BGFX_UNIFORM_TYPE_VEC4;
		break;
	case 'm':
		switch (type[1]) {
		case '3':
			ut = BGFX_UNIFORM_TYPE_MAT3;
			break;
		case '4':
			ut = BGFX_UNIFORM_TYPE_MAT4;
			break;
		default:
			return luaL_error(L, "Invalid Uniform type %s", type);
		}
		break;
	default:
		return luaL_error(L, "Invalid Uniform type %s", type);
	}
	int num = luaL_optinteger(L, 3, 1);
	bgfx_uniform_handle_t handle = BGFX(create_uniform)(name, ut, num);
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create uniform failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE_WITHTYPE(BGFX_LUAHANDLE(UNIFORM, handle), ut));
	return 1;
}

static int
lgetUniformInfo(lua_State *L) {
	uint16_t uniformid = BGFX_LUAHANDLE_ID(UNIFORM, luaL_checkinteger(L, 1));
	bgfx_uniform_handle_t uh = { uniformid };
	bgfx_uniform_info_t ut;
	BGFX(get_uniform_info)(uh, &ut);
	lua_pushstring(L, ut.name);
	switch (ut.type) {
	case BGFX_UNIFORM_TYPE_SAMPLER:
		lua_pushstring(L, "s");
		break;
	case BGFX_UNIFORM_TYPE_VEC4:
		lua_pushstring(L, "v4");
		break;
	case BGFX_UNIFORM_TYPE_MAT3:
		lua_pushstring(L, "m3");
		break;
	case BGFX_UNIFORM_TYPE_MAT4:
		lua_pushstring(L, "m4");
		break;
	default:
		return luaL_error(L, "Invalid uniform:%s, type %d", ut.name, (int)ut.type);
	}
	lua_pushinteger(L, ut.num);
	return 3;
}

static inline int
uniform_size(lua_State *L, int id) {
	int sz;
	switch (BGFX_LUAHANDLE_SUBTYPE(id)) {
	case BGFX_UNIFORM_TYPE_VEC4: sz = 4; break;	// 4 float
	case BGFX_UNIFORM_TYPE_MAT3: sz = 3*4; break;	// 3*4 float
	case BGFX_UNIFORM_TYPE_MAT4: sz = 4*4; break;	// 4*4 float
	default:
		return luaL_error(L, "Invalid uniform type %d", BGFX_LUAHANDLE_SUBTYPE(id));
	}
	return sz;
}

static int
setUniform(lua_State *L, bgfx_uniform_handle_t uh, int sz) {
	int number = lua_gettop(L) - 1;
	int t = lua_type(L, 2);	// the first value type
	switch(t) {
	case LUA_TTABLE: {
		// vector or matrix
		float buffer[V(sz * number)];
		int i,j;
		for (i=0;i<number;i++) {
			luaL_checktype(L, 2+i, LUA_TTABLE);
			for (j=0;j<sz;j++) {
				if (lua_geti(L, 2+i, j+1) != LUA_TNUMBER) {
					return luaL_error(L, "[%d,%d] should be number", i+2,j+1);
				}
				buffer[i*sz+j] = lua_tonumber(L, -1);
				lua_pop(L, 1);
			}
		}
		BGFX(set_uniform)(uh, buffer, number);
		break;
	}
	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		// vector or matrix
		if (number == 1) {
			// only one
			void *data = lua_touserdata(L, 2);
			if (data == NULL)
				return luaL_error(L, "Uniform can't be NULL");
			BGFX(set_uniform)(uh, data, 1);
		} else {
			float buffer[V(sz * number)];
			int i;
			for (i=0;i<number;i++) {
				void * ud = lua_touserdata(L, 2+i);
				if (ud == NULL) {
					return luaL_error(L, "Uniform need userdata at index %d", i+2);
				}
				memcpy(buffer + i * sz, ud, sz*sizeof(float));
			}
			BGFX(set_uniform)(uh, buffer, number);
		}
		break;
	default:
		return luaL_error(L, "Invalid value type : %s", lua_typename(L, t));
	}
	return 0;
}

static int
lsetUniform(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	uint16_t uniformid = BGFX_LUAHANDLE_ID(UNIFORM, id);
	bgfx_uniform_handle_t uh = { uniformid };
	int sz = uniform_size(L, id);
	return setUniform(L, uh, sz);
}

static int
lsetUniformMatrix(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	uint16_t uniformid = BGFX_LUAHANDLE_ID(UNIFORM, id);
	bgfx_uniform_handle_t uh = { uniformid };
	int sz = uniform_size(L, id);
	if (sz <= 4) {
		return luaL_error(L, "Need a matrix");
	}
	return setUniform(L, uh, sz);
}

static int
lsetUniformVector(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	uint16_t uniformid = BGFX_LUAHANDLE_ID(UNIFORM, id);
	bgfx_uniform_handle_t uh = { uniformid };
	int sz = uniform_size(L, id);
	if (sz != 4) {
		return luaL_error(L, "Need a vector");
	}
	return setUniform(L, uh, sz);
}

static uint32_t
border_color_or_compare(lua_State *L, char c) {
	int n  = 0;
	if (c>='0' && c<='9') {
		n = c-'0';
	} else if (c>='a' && c<='f') {
		n = c-'a'+10;
	} else {
		// compare
		switch(c) {
		case '<': return BGFX_SAMPLER_COMPARE_LESS;
		case '[': return BGFX_SAMPLER_COMPARE_LEQUAL;
		case '=': return BGFX_SAMPLER_COMPARE_EQUAL;
		case ']': return BGFX_SAMPLER_COMPARE_GEQUAL;
		case '>': return BGFX_SAMPLER_COMPARE_GREATER;
		case '!': return BGFX_SAMPLER_COMPARE_NOTEQUAL;
		case '-': return BGFX_SAMPLER_COMPARE_NEVER;
		case '+': return BGFX_SAMPLER_COMPARE_ALWAYS;
		default:
			luaL_error(L, "Invalid border color %c", c);
		}
	}
	return BGFX_SAMPLER_BORDER_COLOR(n);
}

static uint64_t
get_texture_flags(lua_State *L, const char *format) {
	int i;
	uint64_t flags = 0;
	for (i=0;format[i];i+=2) {
		int t = 0;
		switch(format[i]) {
		case 'u': t = 0x00; break;	// U
		case 'v': t = 0x10; break;	// V
		case 'w': t = 0x20; break;	// W
		case '-': t = 0x30; break;	// MIN
		case '+': t = 0x40; break;	// MAG
		case '*': t = 0x50; break;	// MIP
		case 'a': t = 0x60; break;	// ALL
		case 'c':
			flags |= border_color_or_compare(L, format[i+1]);
			continue;
		case 'r':	// RT
			switch(format[i+1]) {
			case 't': flags |= BGFX_TEXTURE_RT; break;
			case 'w': flags |= BGFX_TEXTURE_RT_WRITE_ONLY; break;
			case '2': flags |= BGFX_TEXTURE_RT_MSAA_X2; break;
			case '4': flags |= BGFX_TEXTURE_RT_MSAA_X4; break;
			case '8': flags |= BGFX_TEXTURE_RT_MSAA_X8; break;
			case 'x': flags |= BGFX_TEXTURE_RT_MSAA_X16; break;
			default:
				luaL_error(L, "Invalid RT MSAA %c", format[i+1]);
			}
			continue;
		case 'b': // BLIT
			switch(format[i+1]) {
			case 'r' : flags |= BGFX_TEXTURE_READ_BACK; break;
			case 'w' : flags |= BGFX_TEXTURE_BLIT_DST; break;
			case 'c' : flags |= BGFX_TEXTURE_COMPUTE_WRITE; break;
			default:
				luaL_error(L, "Invalid BLIT %c", format[i+1]);
			}
			continue;
		case 's': // SAMPLE
			switch(format[i+1]) {
			case 's': flags |= BGFX_SAMPLER_SAMPLE_STENCIL; break;	//  Sample stencil instead of depth.
			case 'd': break;	// sample depth (by default)
			default:
				luaL_error(L, "Invalid SAMPLE %c", format[i+1]);
			}
			continue;
		case 'S': // sRGB/Gamma space
			switch (format[i + 1]) {
				case 'g': flags |= BGFX_TEXTURE_SRGB; break;// gamma
				case 'l': break;// linear
				default:
					luaL_error(L, "Invalid colorspace flags %c", format[i + 1]);
			}
			continue;
		default:
			luaL_error(L, "Invalid texture flags %c",format[i]);
		}
		switch(format[i+1]) {
		case 'w': t|= 0xf;break;	// WRAP
		case 'm': t|= 1;  break;	// MIRROR
		case 'c': t|= 2;  break;	// CLAMP
		case 'b': t|= 3;  break;	// BORDER
		
		case 'l': t|= 0xe;break;	// LINEAR
		case 'p': t|= 4;  break;	// POINT
		case 'a': t|= 5;  break;	// ANISOTROPIC
		default:
			luaL_error(L, "Invalid texture flags %c", format[i+1]);
		}
		switch(t) {
		case 0x0f:								   break;
		case 0x01: flags |= BGFX_SAMPLER_U_MIRROR; break;
		case 0x02: flags |= BGFX_SAMPLER_U_CLAMP;  break;
		case 0x03: flags |= BGFX_SAMPLER_U_BORDER; break;

		case 0x1f: 									break;
		case 0x11: flags |= BGFX_SAMPLER_V_MIRROR; break;
		case 0x12: flags |= BGFX_SAMPLER_V_CLAMP;  break;
		case 0x13: flags |= BGFX_SAMPLER_V_BORDER; break;

		case 0x2f: 									break;
		case 0x21: flags |= BGFX_SAMPLER_W_MIRROR; break;
		case 0x22: flags |= BGFX_SAMPLER_W_CLAMP;  break;
		case 0x23: flags |= BGFX_SAMPLER_W_BORDER; break;

		case 0x3e: 									break;
		case 0x34: flags |= BGFX_SAMPLER_MIN_POINT; break;
		case 0x35: flags |= BGFX_SAMPLER_MIN_ANISOTROPIC; break;

		case 0x4e: 									break;
		case 0x44: flags |= BGFX_SAMPLER_MAG_POINT; break;
		case 0x45: flags |= BGFX_SAMPLER_MAG_ANISOTROPIC; break;

		case 0x5e: 									break;
		case 0x54: flags |= BGFX_SAMPLER_MIP_POINT; break;

		case 0x6f: 									 break;
		case 0x61: flags |= BGFX_SAMPLER_UVW_MIRROR; break;
		case 0x62: flags |= BGFX_SAMPLER_UVW_CLAMP;  break;
		case 0x63: flags |= BGFX_SAMPLER_UVW_BORDER; break;

		case 0x6e: 								break;
		case 0x64: flags |= BGFX_SAMPLER_POINT; break;
		default:
			luaL_error(L, "Invalid texture flags %c%c", format[i], format[i+1]);
		}
	}
	return flags;
}

static void
parse_texture_info(lua_State *L, int idx, bgfx_texture_info_t *info) {
	if (info->format >= BGFX_TEXTURE_FORMAT_COUNT) {
		luaL_error(L, "Invalid texture format %d", info->format);
	}
	lua_pushstring(L, c_texture_formats[info->format]);
	lua_setfield(L, idx, "format");
	lua_pushinteger(L, info->storageSize);
	lua_setfield(L, idx, "storageSize");
	lua_pushinteger(L, info->width);
	lua_setfield(L, idx, "width");
	lua_pushinteger(L, info->height);
	lua_setfield(L, idx, "height");
	lua_pushinteger(L, info->depth);
	lua_setfield(L, idx, "depth");
	lua_pushinteger(L, info->numLayers);
	lua_setfield(L, idx, "numLayers");
	lua_pushinteger(L, info->numMips);
	lua_setfield(L, idx, "numMips");
	lua_pushinteger(L, info->bitsPerPixel);
	lua_setfield(L, idx, "bitsPerpixel");
	lua_pushboolean(L, info->cubeMap);
	lua_setfield(L, idx, "cubeMap");
}

/*
	integer size	(width * height * 4 for RGBA8888)
 */
static int
lmemoryTexture(lua_State *L) {
	const int intype = lua_type(L, 1);
	if (intype == LUA_TNUMBER) {
		int size = (int)luaL_checkinteger(L, 1);
		void * data = newMemory(L, NULL, size);
		memset(data, 0, size);
		return 1;
	}
	if (intype == LUA_TTABLE) {
		// layout == 'd'
		lua_pushstring(L, "d");
		lua_insert(L, 1);
	}
	return lmemoryBuffer(L);
}

/*
	string imgdata
	string flags	-- [uvw]m/c[-+*]p/a  - MIN + MAG * MIP
	integer skip
	table info
 */
static int
lcreateTexture(lua_State *L) {
	int idx = 2;
	uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE;
	if (lua_type(L, idx) == LUA_TSTRING) {
		const char * f = lua_tostring(L, idx);
		flags = get_texture_flags(L, f);
		++idx;
	}
	int skip = 0;
	if (lua_type(L, idx) == LUA_TNUMBER) {
		skip = lua_tointeger(L, idx);
		++idx;
	}
	const bgfx_memory_t *mem = getMemory(L, 1);
	bgfx_texture_handle_t h;
	if (lua_type(L, idx) == LUA_TTABLE) {
		bgfx_texture_info_t info;
		h = BGFX(create_texture)(mem, flags, skip, &info);
		parse_texture_info(L, idx, &info);
	} else {
		h = BGFX(create_texture)(mem, flags, skip, NULL);
	}
	if (!BGFX_HANDLE_IS_VALID(h)) {
		return luaL_error(L, "create texture failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(TEXTURE, h));

	return 1;
}

static int
lsetName(lua_State *L) {
	int idx = luaL_checkinteger(L, 1);
	int type = idx >> 16;
	int id = idx & 0xffff;
	size_t sz;
	const char *name = luaL_checklstring(L, 2, &sz);
	switch(type) {
	case BGFX_HANDLE_SHADER: {
		bgfx_shader_handle_t handle = { id };
		BGFX(set_shader_name)(handle, name, sz);
		break;
	}
	case BGFX_HANDLE_TEXTURE : {
		bgfx_texture_handle_t handle = { id };
		BGFX(set_texture_name)(handle, name, sz);
		break;
	}
	case BGFX_HANDLE_VERTEX_BUFFER : {
		bgfx_vertex_buffer_handle_t handle = { id };
		BGFX(set_vertex_buffer_name)(handle, name, sz);
		break;
	}
	case BGFX_HANDLE_INDEX_BUFFER : {
		bgfx_index_buffer_handle_t handle = { id };
		BGFX(set_index_buffer_name)(handle, name, sz);
		break;
	}
	case BGFX_HANDLE_FRAME_BUFFER : {
		bgfx_frame_buffer_handle_t handle = { id };
		BGFX(set_frame_buffer_name)(handle, name, sz);
		break;
	}
	default:
		return luaL_error(L, "set_name only support shader , texture, vertex/index/frame buffer.");
	}
	return 0;
}

static uint32_t
texture_sampler_flags(lua_State *L, uint64_t flags) {
	uint32_t sampler = (uint32_t)flags;
	if (sampler != flags) {
		luaL_error(L, "Invalid sampler flags");
	}
	return sampler;
}

static int
lsetTexture(lua_State *L) {
	int stage = luaL_checkinteger(L, 1);
	int uid = luaL_checkinteger(L, 2);
	uint16_t uniform_id = BGFX_LUAHANDLE_ID(UNIFORM, uid);
	if (BGFX_LUAHANDLE_SUBTYPE(uid) != BGFX_UNIFORM_TYPE_SAMPLER) {
		return luaL_error(L, "The uniform is not a sampler");
	}
	uint16_t texture_id = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 3));
	uint64_t flags = UINT32_MAX;
	if (!lua_isnoneornil(L, 4)) {
		const char * f = lua_tostring(L, 4);
		flags = get_texture_flags(L, f);
	}
	bgfx_uniform_handle_t uh = {uniform_id};
	bgfx_texture_handle_t th = {texture_id};

	BGFX(set_texture)(stage, uh, th, texture_sampler_flags(L, flags));

	return 0;
}

static bgfx_frame_buffer_handle_t
create_fb(lua_State *L) {
	int width = luaL_checkinteger(L, 1);
	int height = luaL_checkinteger(L, 2);
	bgfx_texture_format_t fmt = texture_format_from_string(L, 3);
	uint64_t flags = BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP;
	if (!lua_isnoneornil(L, 4)) {
		const char * f = lua_tostring(L, 4);
		flags = get_texture_flags(L, f);
	}
	return BGFX(create_frame_buffer)(width, height, fmt, flags);
}

static bgfx_backbuffer_ratio_t
get_ratio(lua_State *L, int idx) {
	bgfx_backbuffer_ratio_t ratio = 0;
	const char * r = luaL_checkstring(L, idx);
	if (strcmp(r, "1x")==0) {
		ratio = BGFX_BACKBUFFER_RATIO_EQUAL;
	} else if (strcmp(r, "1/2") == 0) {
		ratio = BGFX_BACKBUFFER_RATIO_HALF;
	} else if (strcmp(r, "1/4") == 0) {
		ratio = BGFX_BACKBUFFER_RATIO_QUARTER;
	} else if (strcmp(r, "1/8") == 0) {
		ratio = BGFX_BACKBUFFER_RATIO_EIGHTH;
	} else if (strcmp(r, "1/16") == 0) {
		ratio = BGFX_BACKBUFFER_RATIO_SIXTEENTH;
	} else if (strcmp(r, "2x") == 0) {
		ratio = BGFX_BACKBUFFER_RATIO_DOUBLE;
	} else {
		luaL_error(L, "Invalid back buffer ratio %s", r);
	}
	return ratio;
}

static bgfx_frame_buffer_handle_t
create_fb_scaled(lua_State *L) {
	bgfx_backbuffer_ratio_t ratio = get_ratio(L, 1);
	bgfx_texture_format_t fmt = texture_format_from_string(L, 2);
	uint64_t flags = BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP;
	if (!lua_isnoneornil(L, 3)) {
		const char * f = lua_tostring(L, 3);
		flags = get_texture_flags(L, f);
	}
	return BGFX(create_frame_buffer_scaled)(ratio, fmt, flags);
}

static bgfx_frame_buffer_handle_t
create_fb_mrt(lua_State *L) {
	int n = lua_rawlen(L, 1);
	if (n == 0) {
		luaL_error(L, "At lease one frame buffer");
	}
	int destroy = lua_toboolean(L, 2);
	bgfx_texture_handle_t handles[V(n)];
	int i;
	for (i=0;i<n;i++) {
		if (lua_geti(L, 1, i+1) != LUA_TNUMBER) {
			luaL_error(L, "Invalid texture handle id");
		}
		handles[i].idx = BGFX_LUAHANDLE_ID(TEXTURE, lua_tointeger(L, -1));
		lua_pop(L, 1);
	}
	return BGFX(create_frame_buffer_from_handles)(n, handles, destroy);
}

static bgfx_frame_buffer_handle_t
create_fb_nwh(lua_State *L) {
	void *nwh = lua_touserdata(L, 1);
	int w = luaL_checkinteger(L, 2);
	int h = luaL_checkinteger(L, 3);
	bgfx_texture_format_t fmt = BGFX_TEXTURE_FORMAT_COUNT;
	bgfx_texture_format_t depth_fmt = BGFX_TEXTURE_FORMAT_COUNT;
	if (lua_isstring(L, 4)) {
		fmt = texture_format_from_string(L, 4);
	}
	if (lua_isstring(L, 5)) {
		depth_fmt = texture_format_from_string(L, 5);
	}
	return BGFX(create_frame_buffer_from_nwh)(nwh, w, h, fmt, depth_fmt);
}

/*
	1.
		integer width
		integer height
		string format
		string flags
	2.
		string ratio : 1x 1/2 1/4 1/8 1/16 2x
		string format
		string flags
	3.
		table textures[]
		boolean destroyTex = false
	4.
		lightuserdata window_handle
		integer width
		integer height
	todo:
		from native window handle
		from attachment
 */
static int
lcreateFrameBuffer(lua_State *L) {
	int t = lua_type(L, 1);
	bgfx_frame_buffer_handle_t handle;
	switch(t) {
	case LUA_TNUMBER:
		handle = create_fb(L);
		break;
	case LUA_TTABLE:
		handle = create_fb_mrt(L);
		break;
	case LUA_TSTRING:
		handle = create_fb_scaled(L);
		break;
	case LUA_TLIGHTUSERDATA:
		handle = create_fb_nwh(L);
		break;
	default:
		return luaL_error(L, "Invalid argument type %s for create_frame_buffer", lua_typename(L, t));
	}
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create frame buffer failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(FRAME_BUFFER, handle));
	return 1;
}

/*
	1.
		integer width
		integer height
		boolean hasMips
		integer layers
		string format
		string flags
		todo : mem
	2.
		string radio : 1x 1/2 1/4 1/8 1/16 2x
		boolean hasMips
		integer layers
		string format
		string flags
*/
static int
lcreateTexture2D(lua_State *L) {
	int scaled;
	bgfx_backbuffer_ratio_t	ratio = 0;
	int idx;
	int width=0;
	int height=0;
	if (lua_type(L, 1) == LUA_TSTRING) {
		ratio = get_ratio(L, 1);
		scaled = 1;
		idx = 2;
	} else {
		width = luaL_checkinteger(L, 1);
		height = luaL_checkinteger(L, 2);
		scaled = 0;
		idx = 3;
	}
	int hasMips = lua_toboolean(L, idx++);
	int layers = luaL_checkinteger(L, idx++);
	bgfx_texture_format_t fmt = texture_format_from_string(L, idx++);
	uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE;
	if (!lua_isnoneornil(L, idx)) {
		const char * f = lua_tostring(L, idx++);
		flags = get_texture_flags(L, f);
	}
	bgfx_texture_handle_t handle;
	if (scaled) {
		handle = BGFX(create_texture_2d_scaled)(ratio, hasMips, layers, fmt, flags);
	} else {
		if (width <= 0 || height <= 0) {
			return luaL_error(L, "Invalid texture size (width %d, height %d).", width, height);
		}
		const bgfx_memory_t * mem = NULL;
		if (!lua_isnoneornil(L, idx)) {
			mem = getMemory(L, idx);
		}
		handle = BGFX(create_texture_2d)(width, height, hasMips, layers, fmt, flags, mem);
	}
	if (!BGFX_HANDLE_IS_VALID(handle)) {
		return luaL_error(L, "create texture 2d failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(TEXTURE, handle));
	return 1;
}

/*
	integer texture id
	integer layer
	integer mip
	integer x
	integer y
	integer w
	integer h
	userdata BGFX_MEMORY
	integer pitch = UINT16_MAX
 */
static int
lupdateTexture2D(lua_State *L) {
	uint16_t tid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 1));
	bgfx_texture_handle_t th = { tid };
	int layer = luaL_checkinteger(L, 2);
	int mip = luaL_checkinteger(L, 3);
	int x = luaL_checkinteger(L, 4);
	int y = luaL_checkinteger(L, 5);
	int w = luaL_checkinteger(L, 6);
	int h = luaL_checkinteger(L, 7);
	const bgfx_memory_t *mem = getMemory(L, 8);
	int pitch = luaL_optinteger(L, 9, UINT16_MAX);
	BGFX(update_texture_2d)(th, layer, mip, x, y, w, h, mem, pitch);

	return 0;
}

/*
	integer depth
	boolean cubemap
	integer layers
	string format
	string flags
 */
static int
lisTextureValid(lua_State *L) {
	int depth = luaL_checkinteger(L, 1);
	bool cubemap = lua_toboolean(L, 2);
	int layers = luaL_checkinteger(L, 3);
	bgfx_texture_format_t fmt = texture_format_from_string(L, 4);
	const char * f = luaL_checkstring(L, 5);
	uint64_t flags = get_texture_flags(L, f);

	bool valid = BGFX(is_texture_valid)(depth, cubemap, layers, fmt, flags);
	lua_pushboolean(L, valid);
	return 1;
}

static int
lsetViewOrder(lua_State *L) {
	if (lua_isnoneornil(L, 1)) {
		BGFX(set_view_order)(0,UINT8_MAX,NULL);
		return 0;
	}
	luaL_checktype(L, 1, LUA_TTABLE);
	// todo: set first view not 0
	int n = lua_rawlen(L, 1);
	bgfx_view_id_t order[V(n)];
	int i;
	for (i=0;i<n;i++) {
		if (lua_geti(L, 1, i+1) != LUA_TNUMBER) {
			return luaL_error(L, "Invalid view id");
		}
		order[i] = (bgfx_view_id_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	BGFX(set_view_order)(0,n,order);
	return 0;
}

static int
lsetViewRect(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	int x = luaL_checkinteger(L, 2);
	int y = luaL_checkinteger(L, 3);
	int t = lua_type(L, 4);
	switch(t) {
	case LUA_TSTRING: {
		bgfx_backbuffer_ratio_t ratio = get_ratio(L, 4);
		BGFX(set_view_rect_ratio)(viewid, x, y, ratio);
		break;
	}
	case LUA_TNONE:
	case LUA_TNIL:
		BGFX(set_view_rect_ratio)(viewid, x, y, BGFX_BACKBUFFER_RATIO_EQUAL);
		break;
	case LUA_TNUMBER: {
		int w = luaL_checkinteger(L, 4);
		int h = luaL_checkinteger(L, 5);
		BGFX(set_view_rect)(viewid, x, y, w, h);
		break;
	}
	default:
		return luaL_error(L, "Invalid argument type %s", lua_typename(L, t));
	}
	return 0;
}

static int
lsetViewName(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	const char *name = luaL_checkstring(L, 2);
	BGFX(set_view_name)(viewid, name);
	return 0;
}

static int
lsetViewFrameBuffer(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	uint16_t hid = UINT16_MAX;
	if (!lua_isnoneornil(L, 2)) {
		hid = BGFX_LUAHANDLE_ID(FRAME_BUFFER, luaL_checkinteger(L, 2));
	}
	bgfx_frame_buffer_handle_t h = {hid};
	BGFX(set_view_frame_buffer)(viewid, h);
	return 0;
}

static int
lgetTexture(lua_State *L) {
	uint16_t hid = BGFX_LUAHANDLE_ID(FRAME_BUFFER, luaL_checkinteger(L, 1));
	int attachment = luaL_optinteger(L, 2, 0);

	bgfx_frame_buffer_handle_t h = {hid};
	bgfx_texture_handle_t th = BGFX(get_texture)(h, attachment);
	if (!BGFX_HANDLE_IS_VALID(th)) {
		return luaL_error(L, "get texture failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(TEXTURE, th));
	return 1;
}

/*
	1. 5
		integer id
		integer dst texture handle
		integer dstX
		integer dstY
		integer src texture handle
	2. 9
		...
		integer srcX
		integer srcY
		integer width
		integer height
	3. 7
		integer id
		integer dst texture handle
		integer dstmip
		integer dstX
		integer dstY
		integer dstZ
		integer src texture handle
	4. 14
		...
		integer srcmip
		integer srcX
		integer srcY
		integer srcZ
		integer width
		integer height
		integer depth
 */
static int
lblit(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	uint16_t dstid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 2));
	int top = lua_gettop(L);
	uint16_t dstx;
	uint16_t dsty;
	uint16_t dstz = 0;
	uint16_t srcid;
	uint16_t srcx = 0;
	uint16_t srcy = 0;
	uint16_t srcz = 0;
	uint16_t width = UINT16_MAX;
	uint16_t height = UINT16_MAX;
	uint16_t depth = UINT16_MAX;
	uint8_t dstmip = 0;
	uint8_t srcmip = 0;

	switch(top) {
	case 9:
		srcx = (uint16_t)luaL_checkinteger(L, 6);
		srcy = (uint16_t)luaL_checkinteger(L, 7);
		width = (uint16_t)luaL_checkinteger(L, 8);
		height = (uint16_t)luaL_checkinteger(L, 9);
		// go though
	case 5:
		dstx = (uint16_t)luaL_checkinteger(L, 3);
		dsty = (uint16_t)luaL_checkinteger(L, 4);
		srcid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 5));
		break;
	case 14:
		srcmip = (uint8_t)luaL_checkinteger(L, 8);
		srcx = (uint16_t)luaL_checkinteger(L, 9);
		srcy = (uint16_t)luaL_checkinteger(L, 10);
		srcz = (uint16_t)luaL_checkinteger(L, 11);
		width = (uint16_t)luaL_checkinteger(L, 12);
		height = (uint16_t)luaL_checkinteger(L, 13);
		depth = (uint16_t)luaL_checkinteger(L, 14);
		// go though
	case 7:
		dstmip = (uint8_t)luaL_checkinteger(L, 3);
		dstx = (uint16_t)luaL_checkinteger(L, 4);
		dsty = (uint16_t)luaL_checkinteger(L, 5);
		dstz = (uint16_t)luaL_checkinteger(L, 6);
		srcid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 7));
		break;
	default:
		return luaL_error(L, "Invalid top %d", top);
	}
	bgfx_texture_handle_t sh = { srcid };
	bgfx_texture_handle_t dh = { dstid };

	BGFX(blit)(viewid, dh, dstmip, dstx, dsty, dstz, sh, srcmip, srcx, srcy, srcz, width, height, depth);
	return 0;
}

/*
	integer texture id
	userdata BGFX_MEMORY
	integer mip = 0

	return string
 */
static int
lreadTexture(lua_State *L) {
	uint16_t tid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 1));
	struct memory *mem = (struct memory *)luaL_checkudata(L, 2, "BGFX_MEMORY");
	if (mem->constant) {
		luaL_error(L, "It's constant memory object");
	}
	int mip = luaL_optinteger(L, 3, 0);
	bgfx_texture_handle_t th = { tid };
	int frame = BGFX(read_texture)(th, mem->data, mip);
	lua_pushinteger(L, frame);
	return 1;
}

static uint32_t
stencil_id(lua_State *L) {
	if (lua_type(L, -2) != LUA_TSTRING) {
		luaL_error(L, "stencil key must be string, it's %s", lua_typename(L, lua_type(L, -2)));
	}
	const char * what = lua_tostring(L, -2);
	if CASE(TEST) {
		const char *what = luaL_checkstring(L, -1);
		if CASE(LESS) return BGFX_STENCIL_TEST_LESS;
		if CASE(LEQUAL) return BGFX_STENCIL_TEST_LEQUAL;
		if CASE(EQUAL) return BGFX_STENCIL_TEST_EQUAL;
		if CASE(GEQUAL) return BGFX_STENCIL_TEST_GEQUAL;
		if CASE(GREATER) return BGFX_STENCIL_TEST_GREATER;
		if CASE(NOTEQUAL) return BGFX_STENCIL_TEST_NOTEQUAL;
		if CASE(NEVER) return BGFX_STENCIL_TEST_NEVER;
		if CASE(ALWAYS) return BGFX_STENCIL_TEST_ALWAYS;
		luaL_error(L, "Invalid stencil test %s", what);
	}
	if CASE(FUNC_REF) {
		int r = luaL_checkinteger(L, -1);
		return BGFX_STENCIL_FUNC_REF(r);
	}
	if CASE(FUNC_RMASK) {
		int rmask = luaL_checkinteger(L, -1);
		return BGFX_STENCIL_FUNC_RMASK(rmask);
	}
	uint32_t v = 0;
	do {
		const char * what = luaL_checkstring(L, -1);
		if CASE(ZERO) v = BGFX_STENCIL_OP_FAIL_S_ZERO;
		else if CASE(KEEP) v = BGFX_STENCIL_OP_FAIL_S_KEEP;
		else if CASE(REPLACE) v = BGFX_STENCIL_OP_FAIL_S_REPLACE;
		else if CASE(INCR) v = BGFX_STENCIL_OP_FAIL_S_INCR;
		else if CASE(INCRSAT) v = BGFX_STENCIL_OP_FAIL_S_INCRSAT;
		else if CASE(DECR) v = BGFX_STENCIL_OP_FAIL_S_DECR;
		else if CASE(DECRSAT) v = BGFX_STENCIL_OP_FAIL_S_DECRSAT;
		else if CASE(INVERT) v = BGFX_STENCIL_OP_FAIL_S_INVERT;
		else luaL_error(L, "Invalid stencil op arg %s", what);
	} while(0);
	if CASE(OP_FAIL_Z) {
		v <<= (BGFX_STENCIL_OP_FAIL_Z_SHIFT - BGFX_STENCIL_OP_FAIL_S_SHIFT);
	} else if CASE(OP_PASS_Z) {
		v <<= (BGFX_STENCIL_OP_PASS_Z_SHIFT - BGFX_STENCIL_OP_FAIL_S_SHIFT);
	} else if (!CASE(OP_FAIL_S)) {
		luaL_error(L, "Invalid stencil arg %s", what);
	}
	return v;
}

static int
lmakeStencil(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushnil(L);
	uint32_t stencil = 0;
	while (lua_next(L, 1) != 0) {
		stencil |= stencil_id(L);
		lua_pop(L, 1);
	}
	lua_pushinteger(L, stencil);
	return 1;
}

static int
lsetStencil(lua_State *L) {
	uint32_t fstencil = (uint32_t)luaL_optinteger(L, 1, BGFX_STENCIL_NONE);
	uint32_t bstencil = (uint32_t)luaL_optinteger(L, 2, BGFX_STENCIL_NONE);
	BGFX(set_stencil)(fstencil, bstencil);
	return 0;
}

static int
lsetPaletteColor(lua_State *L) {
	int index = luaL_checkinteger(L, 1);
	int n = lua_gettop(L);
	float c[4];
	if (n == 2) {
		// integer
		uint32_t rgba = (uint32_t)luaL_checkinteger(L, 2);
		int i;
		for (i=3;i>=0;i--) {
			uint8_t v = rgba & 0xff;
			rgba >>= 8;
			c[i] = (float)v / 255.0f;
		}
	} else if (n == 5) {
		// r g b a
		int i;
		for (i=0;i<4;i++) {
			c[i] = luaL_checknumber(L, 2+i);
			if (c[i]<0 || c[i]>1) {
				return luaL_error(L, "Color %f should be in [0,1]", c[i]);
			}
		}
	} else {
		return luaL_error(L, "set_palette_color need 4 float color or 1 uint32");
	}
	BGFX(set_palette_color)(index, c);
	return 0;
}

static int
lsetScissor(lua_State *L) {
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int w = luaL_checkinteger(L, 3);
	int h = luaL_checkinteger(L, 4);
	BGFX(set_scissor)(x,y,w,h);
	return 0;
}

static int
lcreateOcclusionQuery(lua_State *L) {
	bgfx_occlusion_query_handle_t h = BGFX(create_occlusion_query)();
	if (!BGFX_HANDLE_IS_VALID(h)) {
		return luaL_error(L, "create occlusion query failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(OCCLUSION_QUERY, h));
	return 1;
}

static int
lsetCondition(lua_State *L) {
	uint16_t oqid = BGFX_LUAHANDLE_ID(OCCLUSION_QUERY, luaL_checkinteger(L, 1));
	luaL_checktype(L, 2, LUA_TBOOLEAN);
	int visible = lua_toboolean(L, 2);
	bgfx_occlusion_query_handle_t handle = { oqid };
	BGFX(set_condition)(handle, visible);
	return 0;
}

static int
lsubmitOcclusionQuery(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	uint16_t progid = BGFX_LUAHANDLE_ID(PROGRAM, luaL_checkinteger(L, 2));
	uint16_t oqid = BGFX_LUAHANDLE_ID(OCCLUSION_QUERY, luaL_checkinteger(L, 3));
	uint32_t depth = luaL_optinteger(L, 4, 0);
	int preserveState = lua_toboolean(L, 5);
	bgfx_program_handle_t ph = { progid };
	bgfx_occlusion_query_handle_t oqh = { oqid };
	BGFX(submit_occlusion_query)(id, ph, oqh, depth, preserveState);
	return 0;
}

static int
lsubmitIndirect(lua_State *L) {
	int id = luaL_checkinteger(L, 1);
	uint16_t progid = BGFX_LUAHANDLE_ID(PROGRAM, luaL_checkinteger(L, 2));
	uint16_t iid = BGFX_LUAHANDLE_ID(INDIRECT_BUFFER, luaL_checkinteger(L, 3));
	uint16_t start = luaL_optinteger(L, 4, 0);
	uint16_t num = luaL_optinteger(L, 5, 1);
	uint32_t depth = luaL_optinteger(L, 6, 0);
	bool preserveState = lua_toboolean(L, 7);
	bgfx_program_handle_t ph = { progid };
	bgfx_indirect_buffer_handle_t ih = { iid };

	BGFX(submit_indirect)(id, ph, ih, start, num, depth, preserveState);
	return 0;
}

static int
lgetResult(lua_State *L) {
	uint16_t oqid = BGFX_LUAHANDLE_ID(OCCLUSION_QUERY, luaL_checkinteger(L, 1));
	int32_t num=0;
	bgfx_occlusion_query_handle_t oqh = { oqid };
	bgfx_occlusion_query_result_t r = BGFX(get_result)(oqh, &num);
	switch (r) {
	case BGFX_OCCLUSION_QUERY_RESULT_INVISIBLE :
		lua_pushboolean(L, 0);
		break;
	case BGFX_OCCLUSION_QUERY_RESULT_VISIBLE :
		lua_pushboolean(L, 1);
		break;
	case BGFX_OCCLUSION_QUERY_RESULT_NORESULT:
		lua_pushnil(L);
		break;
	default:
		return luaL_error(L, "Invalid result %d", (int)r);
	}
	lua_pushinteger(L, num);
	return 2;
}

static int
lcreateIndirectBuffer(lua_State *L) {
	int num = luaL_checkinteger(L, 1);
	bgfx_indirect_buffer_handle_t h = BGFX(create_indirect_buffer)(num);
	if (!BGFX_HANDLE_IS_VALID(h)) {
		return luaL_error(L, "create occlusion query failed");
	}
	lua_pushinteger(L, BGFX_LUAHANDLE(INDIRECT_BUFFER, h));
	return 1;
}

static bgfx_access_t
access_string(lua_State *L, const char * access) {
	bgfx_access_t a;
	if (access[0] == 'r') {
		a = BGFX_ACCESS_READ;
	} else if (access[0] == 'w') {
		a = BGFX_ACCESS_WRITE;
	} else {
		return luaL_error(L, "Invalid buffer access %s", access);
	}
	if (access[1] != 0) {
		if ((a == BGFX_ACCESS_READ && access[1] == 'w') || (a == BGFX_ACCESS_WRITE && access[1] == 'r'))
			a = BGFX_ACCESS_READWRITE;
		else
			luaL_error(L, "Invalid buffer access %s", access);
	}
	return a;
}

static int
lsetBuffer(lua_State *L) {
	int stage = luaL_checkinteger(L, 1);
	int idx = luaL_checkinteger(L, 2);
	int type = idx >> 16;
	int id = idx & 0xffff;
	const char * access = luaL_checkstring(L, 3);
	bgfx_access_t a = access_string(L, access);
	switch(type) {
	case BGFX_HANDLE_VERTEX_BUFFER: {
		bgfx_vertex_buffer_handle_t handle = { id };
		BGFX(set_compute_vertex_buffer)(stage, handle, a);
		break;
	}
	case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER: {
		bgfx_dynamic_vertex_buffer_handle_t handle = { id };
		BGFX(set_compute_dynamic_vertex_buffer)(stage, handle, a);
		break;
	}
	case BGFX_HANDLE_INDEX_BUFFER: {
		bgfx_index_buffer_handle_t handle = { id };
		BGFX(set_compute_index_buffer)(stage, handle, a);
		break;
	}
	case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER_32:
	case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER: {
		bgfx_dynamic_index_buffer_handle_t handle = { id };
		BGFX(set_compute_dynamic_index_buffer)(stage, handle, a);
		break;
	}
	case BGFX_HANDLE_INDIRECT_BUFFER: {
		bgfx_indirect_buffer_handle_t handle = { id };
		BGFX(set_compute_indirect_buffer)(stage, handle, a);
		break;
	}
	default:
		return luaL_error(L, "Invalid buffer type %d", type);
	}
	return 0;
}

static int
lsetInstanceDataBuffer(lua_State *L) {
	int idx = luaL_checkinteger(L, 1);
	int type = idx >> 16;
	int id = idx & 0xffff;
	uint32_t start = luaL_checkinteger(L, 2);
	uint32_t num = luaL_checkinteger(L, 3);
	switch(type) {
	case BGFX_HANDLE_VERTEX_BUFFER: {
		bgfx_vertex_buffer_handle_t handle = { id };
		BGFX(set_instance_data_from_vertex_buffer)(handle, start, num);
		break;
	}
	case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER: {
		bgfx_dynamic_vertex_buffer_handle_t handle = { id };
		BGFX(set_instance_data_from_dynamic_vertex_buffer)(handle, start, num);
		break;
	}
	default:
		return luaL_error(L, "Invalid set instance data buffer %d", type);
	}
	return 0;
}

static int
lsetInstanceCount(lua_State *L) {
	uint32_t num = luaL_checkinteger(L, 1);
	BGFX(set_instance_count)(num);
	return 0;
}

static int
ldispatch(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	uint16_t pid = BGFX_LUAHANDLE_ID(PROGRAM, luaL_checkinteger(L, 2));
	uint32_t x = luaL_optinteger(L, 3, 1);
	uint32_t y = luaL_optinteger(L, 4, 1);
	uint32_t z = luaL_optinteger(L, 5, 1);
	uint8_t flags = discard_flags(L, 6);

	bgfx_program_handle_t  handle = { pid };

	BGFX(dispatch)(viewid, handle, x, y, z, flags);

	return 0;
}

static int
ldispatchIndirect(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	uint16_t pid = BGFX_LUAHANDLE_ID(PROGRAM, luaL_checkinteger(L, 2));
	uint16_t iid = BGFX_LUAHANDLE_ID(INDIRECT_BUFFER, luaL_checkinteger(L, 3));
	uint16_t start = luaL_optinteger(L, 4, 0);
	uint16_t num = luaL_optinteger(L, 5, 1);
	uint8_t flags = discard_flags(L, 6);
	bgfx_program_handle_t  phandle = { pid };
	bgfx_indirect_buffer_handle_t  ihandle = { iid };

	BGFX(dispatch_indirect)(viewid, phandle, ihandle, start, num, flags);

	return 0;
}

static int
lgetShaderUniforms(lua_State *L) {
	uint16_t sid = BGFX_LUAHANDLE_ID(SHADER, luaL_checkinteger(L, 1));
	bgfx_shader_handle_t shader = { sid };
	uint16_t n = BGFX(get_shader_uniforms)(shader, NULL, 0);
	lua_createtable(L, n, 0);
	bgfx_uniform_handle_t u[V(n)];
	BGFX(get_shader_uniforms)(shader, u, n);	
	for (int i=0;i<n;i++) {
		const bgfx_uniform_handle_t handle = u[i];
		bgfx_uniform_info_t info;
		BGFX(get_uniform_info)(handle, &info);
		const int id = BGFX_LUAHANDLE_WITHTYPE(BGFX_LUAHANDLE(UNIFORM, handle), info.type);
		lua_pushinteger(L, id);
		lua_rawseti(L, -2, i+1);
	}
	return 1;
}

static int
lsetViewMode(lua_State *L) {
	bgfx_view_id_t viewid = luaL_checkinteger(L, 1);
	if (lua_isnoneornil(L, 2)) {
		BGFX(set_view_mode)(viewid, BGFX_VIEW_MODE_DEFAULT);
	} else {
		const char* mode = luaL_checkstring(L, 2);
		switch(mode[0]) {
		case 'd': BGFX(set_view_mode)(viewid, BGFX_VIEW_MODE_DEPTH_ASCENDING); break;
		case 'D': BGFX(set_view_mode)(viewid, BGFX_VIEW_MODE_DEPTH_DESCENDING); break;
		case 's': BGFX(set_view_mode)(viewid, BGFX_VIEW_MODE_SEQUENTIAL); break;
		case '\0':BGFX(set_view_mode)(viewid, BGFX_VIEW_MODE_DEFAULT); break;
		default:
			return luaL_error(L, "Invalid view mode %s", mode);
		}
	}
	return 0;
}

static int
lsetImage(lua_State *L) {
	int stage = luaL_checkinteger(L, 1);
	uint16_t tid = BGFX_LUAHANDLE_ID(TEXTURE, luaL_checkinteger(L, 2));
	bgfx_texture_handle_t handle = { tid };
	int mip = luaL_checkinteger(L, 3);
	bgfx_access_t access = access_string(L, luaL_checkstring(L, 4));
	bgfx_texture_format_t format = 0;
	if (lua_isnoneornil(L, 5)) {
		format = BGFX_TEXTURE_FORMAT_COUNT;
	} else {
		format = texture_format_from_string(L, 5);
	}
	BGFX(set_image)(stage, handle, mip, access, format);
	return 0;
}

static int
lrequestScreenshot(lua_State *L) {
	bgfx_frame_buffer_handle_t handle = { UINT16_MAX };	// Invalid handle (main window)
	if (lua_type(L,1) == LUA_TNUMBER) {
		handle.idx = BGFX_LUAHANDLE_ID(FRAME_BUFFER, luaL_checkinteger(L, 1));
	}
	const char * file = luaL_optstring(L, 2, "");
	BGFX(request_screen_shot)(handle, file);
	return 0;
}

static int
lgetScreenshot(lua_State *L) {
	int memptr = lua_toboolean(L, 1);
	if (lua_getfield(L, LUA_REGISTRYINDEX, "bgfx_cb") != LUA_TUSERDATA) {
		return luaL_error(L, "init first");
	}
	struct callback *cb = lua_touserdata(L, -1);
	struct screenshot * s = ss_pop(&cb->ss);
	if (s == NULL)
		return 0;
	lua_pushstring(L, s->name);
	lua_pushinteger(L, s->width);
	lua_pushinteger(L, s->height);
	lua_pushinteger(L, s->pitch);
	if (memptr) {
		lua_pushlightuserdata(L, s->data);
		lua_pushinteger(L, s->size);
		s->data = NULL;
	} else {
		lua_pushlstring(L, (const char *)s->data, s->size);
	}
	ss_free(s);
	return memptr ? 6 : 5;
}

static int
lgetLog(lua_State *L) {
	if (lua_getfield(L, LUA_REGISTRYINDEX, "bgfx_cb") != LUA_TUSERDATA) {
		return luaL_error(L, "get_log failed!");
	}
	struct callback *cb = lua_touserdata(L, -1);
	struct log_cache *lc = &cb->lc;
	spin_lock(lc);
	int offset = lc->head % MAX_LOGBUFFER;
	int sz = (int)(lc->tail - lc->head);

	int part = MAX_LOGBUFFER - offset;

	if (part >= sz) {
		// only one part
		lua_pushlstring(L, lc->log + offset, sz);
	} else {
		char tmp[MAX_LOGBUFFER];
		memcpy(tmp, lc->log + offset, part);
		memcpy(tmp + part, lc->log, sz - part);
		lua_pushlstring(L, tmp, sz);
	}
	lc->head = lc->tail;

	spin_unlock(lc);
	return 1;
}

LUAMOD_API int
luaopen_bgfx(lua_State *L) {
	luaL_checkversion(L);
	init_interface(L);

	int tfn = sizeof(c_texture_formats) / sizeof(c_texture_formats[0]);
	lua_createtable(L, 0, tfn);
	int i;
	for (i=0;i<tfn;i++) {
		lua_pushstring(L, c_texture_formats[i]);
		lua_pushinteger(L, i);
		lua_settable(L, -3);
	}
	lua_setfield(L, LUA_REGISTRYINDEX, "BGFX_TF");

	luaL_newmetatable(L, "BGFX_IDB");
	luaL_Reg idb[] = {
		{ "alloc", lallocIDB },
		{ "set", lsetIDB },
		{ "pack", lpackIDB },
		{ "format", lformatIDB }, // for math adapter
		{ "__call", lpackIDB },
		{ NULL, NULL },
	};
	luaL_setfuncs(L, idb , 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_Reg l[] = {
		{ "set_platform_data", lsetPlatformData },
		{ "init", linit },
		{ "get_caps", lgetCaps },
		{ "get_stats", lgetStats },
		{ "get_memory", lgetMemory },
		{ "reset", lreset },
		{ "shutdown", lshutdown },
		{ "set_view_clear", lsetViewClear },
		{ "set_view_clear_mrt", lsetViewClearMRT },
		{ "set_view_rect", lsetViewRect },
		{ "set_view_transform", lsetViewTransform },
		{ "set_view_order", lsetViewOrder },
		{ "set_view_name", lsetViewName },
		{ "set_view_frame_buffer", lsetViewFrameBuffer },
		{ "touch", ltouch },
		{ "set_debug", lsetDebug },
		{ "frame", lframe },
		{ "create_shader", lcreateShader },
		{ "create_program", lcreateProgram },
		{ "submit", lsubmit },
		{ "discard", ldiscard },
		{ "make_state", lmakeState },
		{ "set_state", lsetState },
		{ "parse_state", lparseState },
		{ "vertex_layout", lnewVertexLayout },
		{ "vertex_convert", lvertexConvert },
		{ "memory_buffer", lmemoryBuffer},
		{ "calc_tangent", lcalcTangent },
		{ "create_vertex_buffer", lcreateVertexBuffer },
		{ "create_dynamic_vertex_buffer", lcreateDynamicVertexBuffer },
		{ "create_index_buffer", lcreateIndexBuffer },
		{ "create_dynamic_index_buffer", lcreateDynamicIndexBuffer },
		{ "set_vertex_buffer", lsetVertexBuffer },
		{ "set_index_buffer", lsetIndexBuffer },
		{ "destroy", ldestroy },
		{ "set_transform", lsetTransform },
		{ "set_transform_cached", lsetTransformCached },
		{ "set_multi_transforms", lsetMultiTransforms},
		{ "dbg_text_clear", ldbgTextClear },
		{ "dbg_text_print", ldbgTextPrint },
		{ "dbg_text_image", ldbgTextImage },
		{ "transient_buffer", lnewTransientBuffer },
		{ "create_uniform", lcreateUniform },
		{ "get_uniform_info", lgetUniformInfo },
		{ "set_uniform", lsetUniform },
		{ "set_uniform_matrix", lsetUniformMatrix },	// for adapter
		{ "set_uniform_vector", lsetUniformVector },	// for adapter
		{ "instance_buffer", lnewInstanceBuffer },
		{ "instance_buffer_metatable", lgetInstanceBufferMetatable },
		{ "memory_texture", lmemoryTexture },
		{ "create_texture", lcreateTexture },	// create texture from data string (DDS, KTX or PVR texture data)
		{ "create_texture2d", lcreateTexture2D },
		{ "update_texture2d", lupdateTexture2D },
		{ "is_texture_valid", lisTextureValid },
		{ "set_texture", lsetTexture },
		{ "set_name", lsetName },
		{ "create_frame_buffer", lcreateFrameBuffer },
		{ "get_texture", lgetTexture },
		{ "read_texture", lreadTexture },
		{ "blit", lblit },
		{ "make_stencil", lmakeStencil },
		{ "set_stencil", lsetStencil },
		{ "set_palette_color", lsetPaletteColor },
		{ "set_scissor", lsetScissor },
		{ "create_occlusion_query", lcreateOcclusionQuery },
		{ "set_condition", lsetCondition },
		{ "submit_occlusion_query", lsubmitOcclusionQuery },
		{ "get_result", lgetResult },
		{ "create_indirect_buffer", lcreateIndirectBuffer },
		{ "set_buffer", lsetBuffer },
		{ "dispatch", ldispatch },
		{ "dispatch_indirect", ldispatchIndirect },
		{ "set_instance_data_buffer", lsetInstanceDataBuffer },
		{ "set_instance_count", lsetInstanceCount },
		{ "submit_indirect", lsubmitIndirect },
		{ "update", lupdate },
		{ "get_shader_uniforms", lgetShaderUniforms },
		{ "set_view_mode", lsetViewMode },
		{ "set_image", lsetImage },
		{ "request_screenshot", lrequestScreenshot },
		{ "get_screenshot", lgetScreenshot },
		{ "export_vertex_layout", lexportVertexLayout },
		{ "vertex_layout_stride", lvertexLayoutStride },
		{ "get_log", lgetLog },

		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}
