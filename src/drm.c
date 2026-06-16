/*
 * drm.c - Direct DRM/KMS interface built straight on kernel ioctls.
 *
 * The daemon uses libdrm's headers at build time but never links the
 * library: every mode-setting call here is a hand-rolled ioctl wrapper.
 * That keeps the statically linked binary dependency-free at runtime,
 * which matters when it starts from initramfs.
 *
 * Rendering uses two "dumb" buffers flipped front/back. drm_init() saves
 * the CRTC state so drm_cleanup() can hand the display back exactly as it
 * was found; it also force-probes connectors and retries briefly, so it
 * can bring up a cold display - e.g. a USB monitor still enumerating at
 * boot - without another program probing it first.
 */

#include "splash.h"

/*
 * Cold-boot retry budget for drm_init(): how many times, and how far
 * apart, to retry opening the device and finding a usable connector
 * before giving up. 25 x 200 ms = 5 s, enough for a USB display to
 * finish enumerating after a power-on.
 */
#define DRM_INIT_RETRIES   25
#define DRM_INIT_RETRY_MS  200

/* ========================================================================
 * Internal DRM Mode Structures
 *
 * Lightweight stand-ins for libdrm's drmMode* types, populated from the
 * raw ioctl results so the rest of the file can work with plain structs.
 * ======================================================================== */

typedef struct {
	uint32_t crtc_id;
	uint32_t buffer_id;
	uint32_t x, y;
	uint32_t width, height;
	int mode_valid;
	drmModeModeInfo mode;
	int gamma_size;
} _drmModeCrtc;

typedef struct {
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t connector_type;
	uint32_t connector_type_id;
	uint32_t connection;
	uint32_t mmWidth, mmHeight;
	uint32_t subpixel;
	int count_modes;
	int count_props;
	int count_encoders;
	drmModeModeInfo *modes;
	uint32_t *props;
	uint64_t *prop_values;
	uint32_t *encoders;
} _drmModeConnector;

typedef struct {
	uint32_t encoder_id;
	uint32_t encoder_type;
	uint32_t crtc_id;
	uint32_t possible_crtcs;
	uint32_t possible_clones;
} _drmModeEncoder;

typedef struct {
	int count_fbs;
	int count_crtcs;
	int count_connectors;
	int count_encoders;
	uint32_t *fbs;
	uint32_t *crtcs;
	uint32_t *connectors;
	uint32_t *encoders;
	uint32_t min_width, max_width;
	uint32_t min_height, max_height;
} _drmModeRes;

/* ========================================================================
 * Low-level DRM Helpers
 * ======================================================================== */

/* ioctl() wrapper that retries on EINTR/EAGAIN. */
static int _drm_ioctl(int fd, unsigned long request, void *arg) {
	int ret;
	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

/* Zero a struct, including any padding bytes. */
#define ZERO_STRUCT(var) do {            \
		memset(&(var), 0, sizeof(var));  \
	} while (0)

static void _drmModeFreeResources(_drmModeRes *ptr);
static void _drmModeFreeConnector(_drmModeConnector *ptr);

static _drmModeRes *_drmModeGetResources(int fd) {
	struct drm_mode_card_res res;
	uint64_t fbs[64], crtcs[64], connectors[64], encoders[64];

	ZERO_STRUCT(res);
	memset(fbs, 0, sizeof(fbs));
	memset(crtcs, 0, sizeof(crtcs));
	memset(connectors, 0, sizeof(connectors));
	memset(encoders, 0, sizeof(encoders));

	res.fb_id_ptr        = (uint64_t)(uintptr_t)fbs;
	res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
	res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
	res.count_fbs        = 64;
	res.count_crtcs      = 64;
	res.count_connectors = 64;
	res.count_encoders   = 64;

	if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return NULL;

	if (res.count_fbs > 64 || res.count_crtcs > 64 ||
	    res.count_connectors > 64 || res.count_encoders > 64)
		return NULL;

	_drmModeRes *r = calloc(1, sizeof(_drmModeRes));
	if (!r)
		return NULL;

	r->count_fbs        = res.count_fbs;
	r->count_crtcs      = res.count_crtcs;
	r->count_connectors = res.count_connectors;
	r->count_encoders   = res.count_encoders;
	r->min_width        = res.min_width;
	r->max_width        = res.max_width;
	r->min_height       = res.min_height;
	r->max_height       = res.max_height;

	if (res.count_fbs) {
		r->fbs = calloc(res.count_fbs, sizeof(uint32_t));
		if (!r->fbs) goto oom;
		memcpy(r->fbs, fbs, res.count_fbs * sizeof(uint32_t));
	}
	if (res.count_crtcs) {
		r->crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
		if (!r->crtcs) goto oom;
		memcpy(r->crtcs, crtcs, res.count_crtcs * sizeof(uint32_t));
	}
	if (res.count_connectors) {
		r->connectors = calloc(res.count_connectors, sizeof(uint32_t));
		if (!r->connectors) goto oom;
		memcpy(r->connectors, connectors,
		       res.count_connectors * sizeof(uint32_t));
	}
	if (res.count_encoders) {
		r->encoders = calloc(res.count_encoders, sizeof(uint32_t));
		if (!r->encoders) goto oom;
		memcpy(r->encoders, encoders,
		       res.count_encoders * sizeof(uint32_t));
	}

	return r;
oom:
	_drmModeFreeResources(r);
	return NULL;
}

static void _drmModeFreeResources(_drmModeRes *ptr) {
	if (!ptr)
		return;
	free(ptr->fbs);
	free(ptr->crtcs);
	free(ptr->connectors);
	free(ptr->encoders);
	free(ptr);
}

static _drmModeConnector *_drmModeGetConnector(int fd, uint32_t connector_id) {
	/* Oversized, fully zeroed buffer so all struct padding is cleared. */
	union {
		struct drm_mode_get_connector conn;
		unsigned char raw[512];
	} u;
	drmModeModeInfo modes_buf[64];
	uint64_t props_buf[64], propvals_buf[64], encs_buf[64];

	/*
	 * First pass: count_modes == 0 makes the kernel run a full probe -
	 * read the EDID (or fall back to the driver's built-in modes when
	 * there is no EDID) and rebuild the mode list - and report the real
	 * counts. Without this an output that has never been driven, such
	 * as a freshly plugged USB display, reports zero modes and looks
	 * unusable until some other program (a compositor, kmscube, ...)
	 * has probed it first.
	 */
	memset(&u, 0, sizeof(u));
	u.conn.connector_id = connector_id;
	if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &u.conn) < 0)
		return NULL;

	uint32_t n_modes = u.conn.count_modes;
	uint32_t n_props = u.conn.count_props;
	uint32_t n_encs  = u.conn.count_encoders;
	if (n_modes > 64) n_modes = 64;
	if (n_props > 64) n_props = 64;
	if (n_encs  > 64) n_encs  = 64;

	/*
	 * Second pass: hand the kernel buffers sized to the probed counts
	 * and fetch the actual mode / property / encoder lists. A non-zero
	 * count means "copy the cached list", so the probe is not repeated;
	 * connector_id survives from the first pass.
	 */
	memset(modes_buf, 0, sizeof(modes_buf));
	memset(props_buf, 0, sizeof(props_buf));
	memset(propvals_buf, 0, sizeof(propvals_buf));
	memset(encs_buf, 0, sizeof(encs_buf));

	u.conn.modes_ptr       = (uint64_t)(uintptr_t)modes_buf;
	u.conn.props_ptr       = (uint64_t)(uintptr_t)props_buf;
	u.conn.prop_values_ptr = (uint64_t)(uintptr_t)propvals_buf;
	u.conn.encoders_ptr    = (uint64_t)(uintptr_t)encs_buf;
	u.conn.count_modes     = n_modes;
	u.conn.count_props     = n_props;
	u.conn.count_encoders  = n_encs;

	if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &u.conn) < 0)
		return NULL;

	/* The lists can change between the two passes; never trust a count
	 * larger than the buffer that was handed in. */
	uint32_t got_modes = u.conn.count_modes;
	uint32_t got_props = u.conn.count_props;
	uint32_t got_encs  = u.conn.count_encoders;
	if (got_modes > n_modes) got_modes = n_modes;
	if (got_props > n_props) got_props = n_props;
	if (got_encs  > n_encs)  got_encs  = n_encs;

	_drmModeConnector *c = calloc(1, sizeof(_drmModeConnector));
	if (!c)
		return NULL;

	c->connector_id      = u.conn.connector_id;
	c->encoder_id        = u.conn.encoder_id;
	c->connector_type    = u.conn.connector_type;
	c->connector_type_id = u.conn.connector_type_id;
	c->connection        = u.conn.connection;
	c->mmWidth           = u.conn.mm_width;
	c->mmHeight          = u.conn.mm_height;
	c->subpixel          = u.conn.subpixel;
	c->count_modes       = (int)got_modes;
	c->count_props       = (int)got_props;
	c->count_encoders    = (int)got_encs;

	if (got_modes) {
		c->modes = calloc(got_modes, sizeof(drmModeModeInfo));
		if (!c->modes) goto oom;
		memcpy(c->modes, modes_buf,
		       got_modes * sizeof(drmModeModeInfo));
	}
	if (got_props) {
		c->props       = calloc(got_props, sizeof(uint32_t));
		c->prop_values = calloc(got_props, sizeof(uint64_t));
		if (!c->props || !c->prop_values) goto oom;
		memcpy(c->props, props_buf,
		       got_props * sizeof(uint32_t));
		memcpy(c->prop_values, propvals_buf,
		       got_props * sizeof(uint64_t));
	}
	if (got_encs) {
		c->encoders = calloc(got_encs, sizeof(uint32_t));
		if (!c->encoders) goto oom;
		memcpy(c->encoders, encs_buf,
		       got_encs * sizeof(uint32_t));
	}

	return c;
oom:
	_drmModeFreeConnector(c);
	return NULL;
}

static void _drmModeFreeConnector(_drmModeConnector *ptr) {
	if (!ptr)
		return;
	free(ptr->modes);
	free(ptr->props);
	free(ptr->prop_values);
	free(ptr->encoders);
	free(ptr);
}

static _drmModeEncoder *_drmModeGetEncoder(int fd, uint32_t encoder_id) {
	struct drm_mode_get_encoder enc;

	ZERO_STRUCT(enc);
	enc.encoder_id = encoder_id;

	if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
		return NULL;

	_drmModeEncoder *e = calloc(1, sizeof(_drmModeEncoder));
	if (!e)
		return NULL;

	e->encoder_id      = enc.encoder_id;
	e->encoder_type    = enc.encoder_type;
	e->crtc_id         = enc.crtc_id;
	e->possible_crtcs  = enc.possible_crtcs;
	e->possible_clones = enc.possible_clones;

	return e;
}

static void _drmModeFreeEncoder(_drmModeEncoder *ptr) {
	free(ptr);
}

static _drmModeCrtc *_drmModeGetCrtc(int fd, uint32_t crtc_id) {
	struct drm_mode_crtc crtc;

	ZERO_STRUCT(crtc);
	crtc.crtc_id = crtc_id;

	if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0)
		return NULL;

	_drmModeCrtc *c = calloc(1, sizeof(_drmModeCrtc));
	if (!c)
		return NULL;

	c->crtc_id = crtc.crtc_id;
	c->x       = crtc.x;
	c->y       = crtc.y;
	if (crtc.mode_valid) {
		c->width  = crtc.mode.hdisplay;
		c->height = crtc.mode.vdisplay;
	}
	c->mode_valid = crtc.mode_valid;
	c->buffer_id  = crtc.fb_id;
	if (crtc.mode_valid)
		memcpy(&c->mode, &crtc.mode, sizeof(drmModeModeInfo));
	c->gamma_size = crtc.gamma_size;

	return c;
}

static void _drmModeFreeCrtc(_drmModeCrtc *ptr) {
	free(ptr);
}

static int _drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
                           uint32_t x, uint32_t y, uint32_t *connectors,
                           int count, drmModeModeInfo *mode) {
	union {
		struct drm_mode_crtc crtc;
		unsigned char raw[256];
	} u;

	memset(&u, 0, sizeof(u));
	u.crtc.crtc_id            = crtcId;
	u.crtc.fb_id              = bufferId;
	u.crtc.x                  = x;
	u.crtc.y                  = y;
	u.crtc.set_connectors_ptr = (uint64_t)(uintptr_t)connectors;
	u.crtc.count_connectors   = count;
	if (mode) {
		memcpy(&u.crtc.mode, mode, sizeof(struct drm_mode_modeinfo));
		u.crtc.mode_valid = 1;
	}
	return _drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &u.crtc);
}

static int _drmModeAddFB(int fd, uint32_t width, uint32_t height,
                         uint8_t depth, uint8_t bpp, uint32_t pitch,
                         uint32_t bo_handle, uint32_t *buf_id) {
	struct drm_mode_fb_cmd fbcmd;

	ZERO_STRUCT(fbcmd);
	fbcmd.width  = width;
	fbcmd.height = height;
	fbcmd.pitch  = pitch;
	fbcmd.bpp    = bpp;
	fbcmd.depth  = depth;
	fbcmd.handle = bo_handle;

	int ret = _drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd);
	if (ret < 0)
		return ret;
	*buf_id = fbcmd.fb_id;
	return 0;
}

static int _drmModeRmFB(int fd, uint32_t bufferId) {
	return _drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &bufferId);
}

/* ========================================================================
 * CRTC Finding
 * ======================================================================== */

static int find_crtc_for_encoder(_drmModeRes *res, _drmModeEncoder *enc) {
	for (int i = 0; i < res->count_crtcs; i++) {
		if (enc->possible_crtcs & (1 << i))
			return res->crtcs[i];
	}
	return -1;
}

/*
 * Pick the first connected connector with at least one mode, and resolve
 * a CRTC that can drive it. Prefers the encoder/CRTC already bound to the
 * connector, then falls back to any compatible CRTC.
 */
static int find_connector_and_crtc(int fd, uint32_t *conn_id,
                                    uint32_t *crtc_id,
                                    drmModeModeInfo *mode) {
	_drmModeRes *res = _drmModeGetResources(fd);
	if (!res)
		return -1;

	for (int i = 0; i < res->count_connectors; i++) {
		_drmModeConnector *conn =
			_drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;

		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			*conn_id = conn->connector_id;
			memcpy(mode, &conn->modes[0], sizeof(drmModeModeInfo));

			/* Try the encoder already attached to this connector. */
			if (conn->encoder_id) {
				_drmModeEncoder *enc =
					_drmModeGetEncoder(fd, conn->encoder_id);
				if (enc) {
					if (enc->crtc_id) {
						*crtc_id = enc->crtc_id;
						_drmModeFreeEncoder(enc);
						_drmModeFreeConnector(conn);
						_drmModeFreeResources(res);
						return 0;
					}
					int crtc = find_crtc_for_encoder(res, enc);
					if (crtc >= 0) {
						*crtc_id = crtc;
						_drmModeFreeEncoder(enc);
						_drmModeFreeConnector(conn);
						_drmModeFreeResources(res);
						return 0;
					}
					_drmModeFreeEncoder(enc);
				}
			}

			/* Otherwise try every encoder the connector lists. */
			for (int j = 0; j < conn->count_encoders; j++) {
				_drmModeEncoder *enc =
					_drmModeGetEncoder(fd, conn->encoders[j]);
				if (!enc)
					continue;
				int crtc = find_crtc_for_encoder(res, enc);
				if (crtc >= 0) {
					*crtc_id = crtc;
					_drmModeFreeEncoder(enc);
					_drmModeFreeConnector(conn);
					_drmModeFreeResources(res);
					return 0;
				}
				_drmModeFreeEncoder(enc);
			}
		}
		_drmModeFreeConnector(conn);
	}

	_drmModeFreeResources(res);
	return -1;
}

/* ========================================================================
 * Buffer Management
 * ======================================================================== */

/* Create a dumb buffer, register it as a framebuffer, and mmap it. */
static int create_dumb_buffer(int fd, drm_buffer_t *buf,
                               uint32_t width, uint32_t height) {
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	ZERO_STRUCT(creq);
	creq.width  = width;
	creq.height = height;
	creq.bpp    = 32;

	if (_drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;

	buf->width  = width;
	buf->height = height;
	buf->pitch  = creq.pitch;
	buf->handle = creq.handle;
	buf->size   = creq.size;

	if (_drmModeAddFB(fd, width, height, 24, 32, creq.pitch,
	                  creq.handle, &buf->fb_id) < 0)
		goto fail_create;

	ZERO_STRUCT(mreq);
	mreq.handle = creq.handle;
	if (_drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		goto fail_fb;

	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE,
	                MAP_SHARED, fd, mreq.offset);
	if (buf->map == MAP_FAILED)
		goto fail_fb;

	memset(buf->map, 0, buf->size);
	return 0;

fail_fb:
	_drmModeRmFB(fd, buf->fb_id);
fail_create:
	{
		struct drm_mode_destroy_dumb dreq;
		ZERO_STRUCT(dreq);
		dreq.handle = creq.handle;
		_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	return -1;
}

static void destroy_dumb_buffer(int fd, drm_buffer_t *buf) {
	if (buf->map && buf->map != MAP_FAILED) {
		munmap(buf->map, buf->size);
		buf->map = NULL;
	}
	if (buf->fb_id) {
		_drmModeRmFB(fd, buf->fb_id);
		buf->fb_id = 0;
	}
	if (buf->handle) {
		struct drm_mode_destroy_dumb dreq;
		ZERO_STRUCT(dreq);
		dreq.handle = buf->handle;
		_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		buf->handle = 0;
	}
}

/* ========================================================================
 * Public DRM Interface
 * ======================================================================== */

/* Open the DRM device and confirm it answers a version ioctl. */
static int drm_open_device(const char *path) {
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	struct drm_version ver;
	ZERO_STRUCT(ver);
	if (_drm_ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int drm_init(splash_drm_t *ctx, const char *device) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->fd = -1;

	/*
	 * Cold-boot retry. When the splash is the first thing to run at
	 * boot, the DRM device may not exist yet (a USB display still
	 * enumerating) and a just-appeared connector can momentarily report
	 * no modes. Retry the open + connector search for a few seconds so
	 * a cold boot needs no other program to bring the display up first.
	 */
	int fd = -1;
	int ready = 0;
	for (int attempt = 0; attempt < DRM_INIT_RETRIES; attempt++) {
		if (fd < 0)
			fd = drm_open_device(device);

		if (fd >= 0 &&
		    find_connector_and_crtc(fd, &ctx->conn_id,
		                            &ctx->crtc_id, &ctx->mode) == 0) {
			ready = 1;
			break;
		}

		if (attempt + 1 < DRM_INIT_RETRIES) {
			struct timespec ts;
			ts.tv_sec  =  DRM_INIT_RETRY_MS / 1000;
			ts.tv_nsec = (DRM_INIT_RETRY_MS % 1000) * 1000000L;
			nanosleep(&ts, NULL);
		}
	}

	if (!ready) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	ctx->fd = fd;

	uint32_t w = ctx->mode.hdisplay;
	uint32_t h = ctx->mode.vdisplay;

	if (create_dumb_buffer(ctx->fd, &ctx->buf[0], w, h) < 0 ||
	    create_dumb_buffer(ctx->fd, &ctx->buf[1], w, h) < 0) {
		destroy_dumb_buffer(ctx->fd, &ctx->buf[0]);
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	/* Save the current CRTC state so drm_cleanup() can restore it. */
	_drmModeCrtc *crtc = _drmModeGetCrtc(ctx->fd, ctx->crtc_id);
	if (crtc) {
		ctx->saved_crtc.crtc_id    = crtc->crtc_id;
		ctx->saved_crtc.buffer_id  = crtc->buffer_id;
		ctx->saved_crtc.x          = crtc->x;
		ctx->saved_crtc.y          = crtc->y;
		ctx->saved_crtc.mode_valid = crtc->mode_valid;
		if (crtc->mode_valid)
			memcpy(&ctx->saved_crtc.mode, &crtc->mode,
			       sizeof(drmModeModeInfo));
		_drmModeFreeCrtc(crtc);
	}

	/* Scan out the first buffer. */
	uint32_t conn = ctx->conn_id;
	_drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->buf[0].fb_id,
	                0, 0, &conn, 1, &ctx->mode);
	ctx->front_buf = 0;

	return 0;
}

void drm_cleanup(splash_drm_t *ctx) {
	/* Hand the display back to whatever owned it before. */
	if (ctx->saved_crtc.mode_valid) {
		uint32_t conn = ctx->conn_id;
		_drmModeSetCrtc(ctx->fd, ctx->saved_crtc.crtc_id,
		                ctx->saved_crtc.buffer_id,
		                ctx->saved_crtc.x, ctx->saved_crtc.y,
		                &conn, 1, &ctx->saved_crtc.mode);
	}
	destroy_dumb_buffer(ctx->fd, &ctx->buf[0]);
	destroy_dumb_buffer(ctx->fd, &ctx->buf[1]);
	if (ctx->fd >= 0) {
		close(ctx->fd);
		ctx->fd = -1;
	}
}

/* Present the back buffer and make it the new front buffer. */
void drm_flip(splash_drm_t *ctx) {
	int next = ctx->front_buf ^ 1;
	uint32_t conn = ctx->conn_id;
	_drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->buf[next].fb_id,
	                0, 0, &conn, 1, &ctx->mode);
	ctx->front_buf = next;
}
