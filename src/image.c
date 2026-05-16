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

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	off_t sz = lseek(fd, 0, SEEK_END);
	if (sz <= 0 || sz > MAX_IMAGE_FILE_SIZE) {
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);

	uint8_t *data = malloc((size_t)sz);
	if (!data) {
		close(fd);
		return -1;
	}

	/* read() may return fewer bytes than asked, so loop until complete. */
	size_t got = 0;
	while (got < (size_t)sz) {
		ssize_t n = read(fd, data + got, (size_t)sz - got);
		if (n <= 0) {				/* error or unexpected EOF */
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

	return img->rgba ? 0 : -1;
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
