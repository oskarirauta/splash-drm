/*
 * drm.c - Direct DRM/KMS interface using kernel ioctls
 * 
 * Reimplements minimal libdrm functionality for zero external dependencies.
 */

#include "splash.h"

/* ========================================================================
 * Internal DRM Mode Structures (matching kernel UAPI)
 * ======================================================================== */

typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} _drmModeModeInfo;

typedef struct {
    uint32_t crtc_id;
    uint32_t buffer_id;
    uint32_t x, y;
    uint32_t width, height;
    int mode_valid;
    _drmModeModeInfo mode;
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
    _drmModeModeInfo *modes;
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

static int _drm_ioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

static _drmModeRes* _drmModeGetResources(int fd) {
    struct drm_mode_card_res res = {0};
    uint64_t fbs[64], crtcs[64], connectors[64], encoders[64];
    
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encoders;
    res.count_fbs = 64;
    res.count_crtcs = 64;
    res.count_connectors = 64;
    res.count_encoders = 64;
    
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return NULL;
    
    if (res.count_fbs > 64 || res.count_crtcs > 64 || 
        res.count_connectors > 64 || res.count_encoders > 64)
        return NULL;
    
    _drmModeRes *r = calloc(1, sizeof(_drmModeRes));
    if (!r) return NULL;
    
    r->count_fbs = res.count_fbs;
    r->count_crtcs = res.count_crtcs;
    r->count_connectors = res.count_connectors;
    r->count_encoders = res.count_encoders;
    r->min_width = res.min_width;
    r->max_width = res.max_width;
    r->min_height = res.min_height;
    r->max_height = res.max_height;
    
    if (res.count_fbs) {
        r->fbs = calloc(res.count_fbs, sizeof(uint32_t));
        memcpy(r->fbs, fbs, res.count_fbs * sizeof(uint32_t));
    }
    if (res.count_crtcs) {
        r->crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
        memcpy(r->crtcs, crtcs, res.count_crtcs * sizeof(uint32_t));
    }
    if (res.count_connectors) {
        r->connectors = calloc(res.count_connectors, sizeof(uint32_t));
        memcpy(r->connectors, connectors, res.count_connectors * sizeof(uint32_t));
    }
    if (res.count_encoders) {
        r->encoders = calloc(res.count_encoders, sizeof(uint32_t));
        memcpy(r->encoders, encoders, res.count_encoders * sizeof(uint32_t));
    }
    
    return r;
}

static void _drmModeFreeResources(_drmModeRes *ptr) {
    if (!ptr) return;
    free(ptr->fbs);
    free(ptr->crtcs);
    free(ptr->connectors);
    free(ptr->encoders);
    free(ptr);
}

static _drmModeConnector* _drmModeGetConnector(int fd, uint32_t connector_id) {
    struct drm_mode_get_connector conn = {0};
    uint64_t modes_buf[64], props_buf[64], propvals_buf[64], encs_buf[64];
    
    conn.connector_id = connector_id;
    conn.modes_ptr = (uint64_t)(uintptr_t)modes_buf;
    conn.props_ptr = (uint64_t)(uintptr_t)props_buf;
    conn.prop_values_ptr = (uint64_t)(uintptr_t)propvals_buf;
    conn.encoders_ptr = (uint64_t)(uintptr_t)encs_buf;
    conn.count_modes = 64;
    conn.count_props = 64;
    conn.count_encoders = 64;
    
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
        return NULL;
    
    if (conn.count_modes > 64 || conn.count_props > 64 || conn.count_encoders > 64)
        return NULL;
    
    _drmModeConnector *c = calloc(1, sizeof(_drmModeConnector));
    if (!c) return NULL;
    
    c->connector_id = conn.connector_id;
    c->encoder_id = conn.encoder_id;
    c->connector_type = conn.connector_type;
    c->connector_type_id = conn.connector_type_id;
    c->connection = conn.connection;
    c->mmWidth = conn.mm_width;
    c->mmHeight = conn.mm_height;
    c->subpixel = conn.subpixel;
    c->count_modes = conn.count_modes;
    c->count_props = conn.count_props;
    c->count_encoders = conn.count_encoders;
    
    if (conn.count_modes) {
        c->modes = calloc(conn.count_modes, sizeof(_drmModeModeInfo));
        memcpy(c->modes, modes_buf, conn.count_modes * sizeof(_drmModeModeInfo));
    }
    if (conn.count_props) {
        c->props = calloc(conn.count_props, sizeof(uint32_t));
        c->prop_values = calloc(conn.count_props, sizeof(uint64_t));
        memcpy(c->props, props_buf, conn.count_props * sizeof(uint32_t));
        memcpy(c->prop_values, propvals_buf, conn.count_props * sizeof(uint64_t));
    }
    if (conn.count_encoders) {
        c->encoders = calloc(conn.count_encoders, sizeof(uint32_t));
        memcpy(c->encoders, encs_buf, conn.count_encoders * sizeof(uint32_t));
    }
    
    return c;
}

static void _drmModeFreeConnector(_drmModeConnector *ptr) {
    if (!ptr) return;
    free(ptr->modes);
    free(ptr->props);
    free(ptr->prop_values);
    free(ptr->encoders);
    free(ptr);
}

static _drmModeEncoder* _drmModeGetEncoder(int fd, uint32_t encoder_id) {
    struct drm_mode_get_encoder enc = {0};
    enc.encoder_id = encoder_id;
    
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
        return NULL;
    
    _drmModeEncoder *e = calloc(1, sizeof(_drmModeEncoder));
    if (!e) return NULL;
    
    e->encoder_id = enc.encoder_id;
    e->encoder_type = enc.encoder_type;
    e->crtc_id = enc.crtc_id;
    e->possible_crtcs = enc.possible_crtcs;
    e->possible_clones = enc.possible_clones;
    
    return e;
}

static void _drmModeFreeEncoder(_drmModeEncoder *ptr) {
    free(ptr);
}

static _drmModeCrtc* _drmModeGetCrtc(int fd, uint32_t crtc_id) {
    struct drm_mode_crtc crtc = {0};
    crtc.crtc_id = crtc_id;
    
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0)
        return NULL;
    
    _drmModeCrtc *c = calloc(1, sizeof(_drmModeCrtc));
    if (!c) return NULL;
    
    c->crtc_id = crtc.crtc_id;
    c->x = crtc.x;
    c->y = crtc.y;
    c->width = crtc.width;
    c->height = crtc.height;
    c->mode_valid = crtc.mode_valid;
    c->buffer_id = crtc.fb_id;
    if (crtc.mode_valid)
        memcpy(&c->mode, &crtc.mode, sizeof(_drmModeModeInfo));
    c->gamma_size = crtc.gamma_size;
    
    return c;
}

static void _drmModeFreeCrtc(_drmModeCrtc *ptr) {
    free(ptr);
}

static int _drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
                            uint32_t x, uint32_t y, uint32_t *connectors, int count,
                            _drmModeModeInfo *mode) {
    struct drm_mode_crtc_set crtc = {0};
    crtc.crtc_id = crtcId;
    crtc.fb_id = bufferId;
    crtc.x = x;
    crtc.y = y;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)connectors;
    crtc.count_connectors = count;
    if (mode) {
        memcpy(&crtc.mode, mode, sizeof(struct drm_mode_modeinfo));
        crtc.mode_valid = 1;
    }
    return _drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
}

static int _drmModeAddFB(int fd, uint32_t width, uint32_t height, uint8_t depth,
                        uint8_t bpp, uint32_t pitch, uint32_t bo_handle,
                        uint32_t *buf_id) {
    struct drm_mode_fb_cmd fbcmd = {0};
    fbcmd.width = width;
    fbcmd.height = height;
    fbcmd.pitch = pitch;
    fbcmd.bpp = bpp;
    fbcmd.depth = depth;
    fbcmd.handle = bo_handle;
    
    int ret = _drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd);
    if (ret < 0) return ret;
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

static int find_connector_and_crtc(int fd, uint32_t *conn_id, uint32_t *crtc_id, 
                                    drm_mode_info_t *mode) {
    _drmModeRes *res = _drmModeGetResources(fd);
    if (!res) return -1;
    
    for (int i = 0; i < res->count_connectors; i++) {
        _drmModeConnector *conn = _drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;
        
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            *conn_id = conn->connector_id;
            memcpy(mode, &conn->modes[0], sizeof(drm_mode_info_t));
            
            if (conn->encoder_id) {
                _drmModeEncoder *enc = _drmModeGetEncoder(fd, conn->encoder_id);
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
            
            for (int j = 0; j < conn->count_encoders; j++) {
                _drmModeEncoder *enc = _drmModeGetEncoder(fd, conn->encoders[j]);
                if (!enc) continue;
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

static int create_dumb_buffer(int fd, drm_buffer_t *buf, uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb creq = {0};
    creq.width = width;
    creq.height = height;
    creq.bpp = 32;
    
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;
    
    buf->width = width;
    buf->height = height;
    buf->pitch = creq.pitch;
    buf->handle = creq.handle;
    buf->size = creq.size;
    
    if (_drmModeAddFB(fd, width, height, 24, 32, creq.pitch, creq.handle, &buf->fb_id) < 0)
        goto fail_create;
    
    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = creq.handle;
    if (_drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        goto fail_fb;
    
    buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (buf->map == MAP_FAILED)
        goto fail_fb;
    
    memset(buf->map, 0, buf->size);
    return 0;
    
fail_fb:
    _drmModeRmFB(fd, buf->fb_id);
fail_create:
    {
        struct drm_mode_destroy_dumb dreq = {.handle = creq.handle};
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
        struct drm_mode_destroy_dumb dreq = {.handle = buf->handle};
        _drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        buf->handle = 0;
    }
}

/* ========================================================================
 * Public DRM Interface
 * ======================================================================== */

static int drm_open_device(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    
    struct drm_version ver = {0};
    if (_drm_ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int drm_init(drm_context_t *ctx, const char *device) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = drm_open_device(device);
    if (ctx->fd < 0) return -1;
    
    if (find_connector_and_crtc(ctx->fd, &ctx->conn_id, &ctx->crtc_id, &ctx->mode) < 0) {
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    
    uint32_t w = ctx->mode.hdisplay;
    uint32_t h = ctx->mode.vdisplay;
    
    if (create_dumb_buffer(ctx->fd, &ctx->buf[0], w, h) < 0 ||
        create_dumb_buffer(ctx->fd, &ctx->buf[1], w, h) < 0) {
        destroy_dumb_buffer(ctx->fd, &ctx->buf[0]);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    
    /* Save current CRTC state for cleanup */
    _drmModeCrtc *crtc = _drmModeGetCrtc(ctx->fd, ctx->crtc_id);
    if (crtc) {
        ctx->saved_crtc.crtc_id = crtc->crtc_id;
        ctx->saved_crtc.buffer_id = crtc->buffer_id;
        ctx->saved_crtc.x = crtc->x;
        ctx->saved_crtc.y = crtc->y;
        ctx->saved_crtc.mode_valid = crtc->mode_valid;
        if (crtc->mode_valid)
            memcpy(&ctx->saved_crtc.mode, &crtc->mode, sizeof(drm_mode_info_t));
        _drmModeFreeCrtc(crtc);
    }
    
    /* Set initial mode */
    uint32_t conn = ctx->conn_id;
    _drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->buf[0].fb_id, 0, 0, &conn, 1, 
                    (_drmModeModeInfo*)&ctx->mode);
    ctx->front_buf = 0;
    
    return 0;
}

void drm_cleanup(drm_context_t *ctx) {
    if (ctx->saved_crtc.mode_valid) {
        uint32_t conn = ctx->conn_id;
        _drmModeSetCrtc(ctx->fd, ctx->saved_crtc.crtc_id, ctx->saved_crtc.buffer_id,
                       ctx->saved_crtc.x, ctx->saved_crtc.y, &conn, 1,
                       (_drmModeModeInfo*)&ctx->saved_crtc.mode);
    }
    destroy_dumb_buffer(ctx->fd, &ctx->buf[0]);
    destroy_dumb_buffer(ctx->fd, &ctx->buf[1]);
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

void drm_flip(drm_context_t *ctx) {
    int next = ctx->front_buf ^ 1;
    uint32_t conn = ctx->conn_id;
    _drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->buf[next].fb_id, 0, 0, &conn, 1,
                    (_drmModeModeInfo*)&ctx->mode);
    ctx->front_buf = next;
}
