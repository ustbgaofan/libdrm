#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <xf86drm.h>

#include <tegra.h>

static int gr2d_fill(struct host1x_pushbuf *pb, struct drm_tegra_bo *dst,
                     int pitch, uint32_t color, int x, int y, int w, int h)
{
	uint32_t controlmain;

#define PUSH(x) \
	if (host1x_pushbuf_push(pb, x) < 0) { \
		fprintf(stderr, "push failed!\n"); \
		return -1; \
	}

	PUSH(HOST1X_OPCODE_SETCL(0, HOST1X_CLASS_GR2D, 0));

	PUSH(HOST1X_OPCODE_MASK(0x9, 0x9));
	PUSH(0x0000003a);  /* 0x009 - trigger */
	PUSH(0x00000000);  /* 0x00c - cmdsel */

	PUSH(HOST1X_OPCODE_MASK(0x1e, 0x7));
	PUSH(0x00000000);  /* 0x01e - controlsecond */
	controlmain  = 1 << 6;  /* fill mode */
	controlmain |= 1 << 2;  /* turbofill */
	controlmain |= 2 << 16; /* 4 bytes per pixel */
	PUSH(controlmain); /* 0x01f - controlmain */
	PUSH(0x000000cc);  /* 0x020 - ropfade */

	PUSH(HOST1X_OPCODE_MASK(0x2b, 0x9));
	if (host1x_pushbuf_relocate(pb, dst, 0, 0) < 0) {
		fprintf(stderr, "relocate failed\n");
		return -1;
	}
	PUSH(0xdeadbeef);  /* 0x02b - dstba */
	PUSH(pitch);       /* 0x02e - dstst */

	PUSH(HOST1X_OPCODE_NONINCR(0x35, 1));
	PUSH(color);       /* 0x035 - srcfgc */

	PUSH(HOST1X_OPCODE_NONINCR(0x46, 1));
	PUSH(0);           /* 0x046 - tilemode */

	PUSH(HOST1X_OPCODE_MASK(0x38, 0x5));
	PUSH(h << 16 | w); /* 0x038 - dstsize */
	PUSH(y << 16 | x); /* 0x03a - dstps */

#undef PUSH

	return 0;
}

static int test_gr2d_fill(struct drm_tegra *drm, struct drm_tegra_channel *channel)
{
	int err, i, clears = 16;
	struct host1x_pushbuf *pb;
	struct host1x_job *job;
	struct drm_tegra_bo *cmdbuf, *dst;
	struct host1x_fence *fence;
	uint32_t *pixels;

	static const uint32_t colors[4] = {
		0xFF00FF00,
		0x00FF00FF,
		0xFFFF0000,
		0x0000FFFF
	};

	err = host1x_job_create(channel, &job);
	if (err < 0) {
		fprintf(stderr, "host1x_job_create() failed: %d\n", err);
		return -1;
	}
	assert(job);

	err = drm_tegra_bo_create(drm, 0, 32 * 4096, &cmdbuf);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_bo_create() failed: %d\n", err);
		return -1;
	}
	assert(cmdbuf);

	err = drm_tegra_bo_create(drm, 0, 4 * 2048 * 2048, &dst);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_bo_create() failed: %d\n", err);
		return -1;
	}
	assert(dst);

	err = host1x_job_append(job, cmdbuf, 0, &pb);
	if (err < 0) {
		fprintf(stderr, "host1x_job_append() failed: %d\n", err);
		return -1;
	}
	assert(pb);

	for (i = 0; i < clears; ++i) {
		int x, y, w, h;
		uint32_t color = colors[i & 3];

		x = y = i;
		w = h = 2048 - i * 2;
		err = gr2d_fill(pb, dst, 4 * 2048, color, x, y, w, h);
		if (err < 0) {
			fprintf(stderr, "gr2d_fill() failed: %d\n", err);
			return -1;
		}
	}

	err = host1x_pushbuf_push(pb, HOST1X_OPCODE_NONINCR(0x00, 1));
	if (err < 0) {
		fprintf(stderr, "host1x_pushbuf_push() failed: %d\n", err);
		return -1;
	}

	err = host1x_pushbuf_sync(pb, HOST1X_SYNCPT_COND_OP_DONE);
	if (err < 0) {
		fprintf(stderr, "host1x_pushbuf_sync() failed: %d\n", err);
		return -1;
	}

	err = drm_tegra_submit(drm, job, &fence);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_submit() failed: %d\n", err);
		return -1;
	}
	assert(fence);

	err = drm_tegra_wait(drm, fence, 15000);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_wait() failed: %d\n", err);
		return -1;
	}

	drm_tegra_bo_map(dst, (void **)&pixels);
	assert((((intptr_t)pixels) & 3) == 0);

	for (i = 0; i < clears; ++i) {
		uint32_t color = colors[i & 3];

#define CMP(x, y) \
do { \
	if (pixels[(x) + (y) * 2048] != color) { \
		fprintf(stderr, "(%d, %d) : expected %08x, got %08x\n", \
		    x, y, color, pixels[x]); \
		return -1; \
	} \
} while (0)

		/* check that the corners of each clear-rectangle matches */
		CMP(i,        i);
		CMP(2047 - i, i);
		CMP(i,        2047 - i);
		CMP(2047 - i, 2047 - i);

		/* check that the mid-edges of each clear-rectangle matches */
		CMP(1024,     i);
		CMP(1024,     2047 - i);
		CMP(i,        1024);
		CMP(2047 - i, 1024);

#undef CMP
	}

	return 0;
}

int main()
{
	int fd, err;
	struct drm_tegra *drm;
	struct drm_tegra_channel *channel;

	fd = drmOpen("tegra", NULL);
	if (fd < 0) {
		fprintf(stderr, "drmOpem failed\n");
		return -1;
	}

	err = drm_tegra_open(fd, &drm);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_open() failed: %d\n", err);
		return -1;
	}
	assert(drm);

	err = drm_tegra_channel_open(drm, HOST1X_CLASS_GR2D, &channel);
	if (err < 0) {
		fprintf(stderr, "drm_tegra_channel_open failed: %d\n", err);
		return -1;
	}
	assert(channel);

	err = test_gr2d_fill(drm, channel);
	if (err < 0) {
		fprintf(stderr, "test_gr2d_fill failed\n");
		return -1;
	}

	return 0;
}