/*
 * image.c - PNG image loading via stb_image.
 *
 * STBI_ONLY_PNG keeps just the PNG decoder (the only format the splash
 * uses), and STBI_NO_STDIO means stb_image never touches FILE* - the
 * file is read here and handed to stb as an in-memory buffer.
 */

#include "splash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

/*
 * Reject absurdly large files before allocating. A splash PNG is
 * realistically well under a megabyte; 64 MiB is generous headroom and
 * also keeps the file size within range of the (int) length argument
 * that stb_image expects.
 */
#define MAX_IMAGE_FILE_SIZE (64 * 1024 * 1024)

/* ========================================================================
 * Image Loading
 * ======================================================================== */

int load_image(const char *path, image_t *img) {
	if (!path || !img)
		return -1;

	if (g_validate_only)
		return 0;		/* --check: validate the command, skip the file */

	char resolved[PATH_MAX];
	if (resolve_image_path(path, resolved, sizeof(resolved)) == 0)
		path = resolved;

	/* O_NONBLOCK so opening a client-supplied path that happens to be a FIFO
	 * or device never blocks here. */
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		LOGW("image: cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	/* Only ever read a regular file: a path pointing at a FIFO/device/socket
	 * could otherwise wedge the single-threaded daemon forever, or hand stb a
	 * non-seekable stream. */
	struct stat stbuf;
	if (fstat(fd, &stbuf) < 0 || !S_ISREG(stbuf.st_mode)) {
		LOGW("image: %s is not a regular file", path);
		close(fd);
		return -1;
	}

	off_t sz = lseek(fd, 0, SEEK_END);
	if (sz <= 0 || sz > MAX_IMAGE_FILE_SIZE) {
		LOGW("image: %s has unusable size (%lld bytes)", path, (long long)sz);
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);

	uint8_t *data = malloc((size_t)sz);
	if (!data) {
		close(fd);
		return -1;			/* OOM: caller degrades, keep quiet */
	}

	/* read() may return fewer bytes than asked, so loop until complete. */
	size_t got = 0;
	while (got < (size_t)sz) {
		ssize_t n = read(fd, data + got, (size_t)sz - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {				/* error or unexpected EOF */
			LOGW("image: short read on %s", path);
			free(data);
			close(fd);
			return -1;
		}
		got += (size_t)n;
	}
	close(fd);

	int comp;
	img->rgba = stbi_load_from_memory(data, (int)sz, &img->w, &img->h,
	                                  &comp, 4);
	free(data);

	if (!img->rgba) {
		LOGW("image: failed to decode %s (not a valid PNG?)", path);
		return -1;
	}
	return 0;
}

/* ========================================================================
 * Image Cleanup
 * ======================================================================== */

void free_image(image_t *img) {
	if (!img)
		return;

	if (img->rgba) {
		stbi_image_free(img->rgba);
		img->rgba = NULL;
	}
	img->w = 0;
	img->h = 0;
}

/* ========================================================================
 * Framebuffer Screenshot (--dump)
 * ======================================================================== */

/*
 * Write a rendered framebuffer to a PNG. The buffer holds 0xAARRGGBB pixels
 * (native uint32), so we emit opaque 24-bit RGB (the X/alpha byte is not part
 * of what reaches the screen), walking rows by `pitch` since it may exceed
 * width*4. Used by --dump to capture a frame for inspection / golden tests.
 */
int write_buffer_png(const drm_buffer_t *buf, const char *path) {
	if (!buf || !buf->map || buf->width == 0 || buf->height == 0)
		return -1;

	int w = (int)buf->width, h = (int)buf->height;
	uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
	if (!rgb)
		return -1;

	for (int y = 0; y < h; y++) {
		const uint32_t *row =
		    (const uint32_t *)(buf->map + (size_t)y * buf->pitch);
		uint8_t *out = rgb + (size_t)y * (size_t)w * 3;
		for (int x = 0; x < w; x++) {
			uint32_t px = row[x];
			out[x * 3 + 0] = (px >> 16) & 0xFF;	/* R */
			out[x * 3 + 1] = (px >> 8)  & 0xFF;	/* G */
			out[x * 3 + 2] =  px        & 0xFF;	/* B */
		}
	}

	int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
	free(rgb);
	return ok ? 0 : -1;
}
