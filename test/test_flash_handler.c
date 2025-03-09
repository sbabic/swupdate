/*
 * Copyright Viacheslav Volkov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdint.h>
#include <libmtd.h>
#include <mtd/mtd-user.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "util.h"
#include "pctl.h"
#include "flash.h"
#include "handler.h"
#include "swupdate_image.h"
#include "swupdate_status.h"

/* #define PRINT_LOGS */
#define RETURN_CODE_ENOSPC
#define DEV_MTD_PREFIX "/dev/mtd"
#define MTD_DEV_IDX 0
/* MTD_FD is a big dummy value: it is very unlikely for us to have so many open
 * file descriptors: */
#define MTD_FD 999999
#define LIBMTD_T_VALUE ((libmtd_t)0x12345678) /* dummy value */

#define FOREACH_PAGE_IN_EB(page, mtd, eb) \
	for (page = (eb) * ((mtd)->eb_size / (mtd)->min_io_size); \
	     page < (eb + 1) * ((mtd)->eb_size / (mtd)->min_io_size); \
	     page++)

#define FOREACH_PAGE_WRITTEN(page, mtd, eb, offs, len) \
	for (page = ((eb) * (mtd)->eb_size + (offs)) / (mtd)->min_io_size; \
	page < ((eb) * (mtd)->eb_size + (offs) + (len)) / (mtd)->min_io_size; \
	     page++)

/*
 * Mock data
 */
static struct img_type image = {
	.type = "flash",
};
static struct mtd_ubi_info mtd_ubi_info_s;

/*
 * Test-only data
 */
static handler handler_func;
static bool is_nand;
static unsigned char *image_buf;
static void (*patch_image_buf)(unsigned char *buf);
static int eb_bytes;
static int pages_bytes;
/* Actual flash state before test run: */
static unsigned char *bad_blocks;
static unsigned char *locked_blocks;
static unsigned char *written_pages;
static unsigned char *flash_memory;
/* Expected flash state after test run: */
static unsigned char *expected_bad_blocks;
static unsigned char *expected_locked_blocks;
static unsigned char *expected_written_pages;
static unsigned char *expected_flash_memory;

void *__real_malloc(size_t size);
static void *(*impl_malloc)(size_t size) = __real_malloc;
void *__wrap_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
	return impl_malloc(size);
}

int __real_open(const char *pathname, int flags);
static int default_open(const char *pathname, int flags)
{
	int ret;
	if (strcmp(pathname, image.device) == 0)
		return MTD_FD;
	ret = __real_open(pathname, flags);
	/* Usually file descriptors start from 0. Since MTD_FD is big enough,
	 * a collision is very unlikely. */
	assert_int_not_equal(ret, MTD_FD);
	return ret;
}
static int (*impl_open)(const char *pathname, int flags) = default_open;
int __wrap_open(const char *pathname, int flags);
int __wrap_open(const char *pathname, int flags)
{
	return impl_open(pathname, flags);
}

int __real_close(int fd);
int __wrap_close(int fd);
int __wrap_close(int fd)
{
	if (fd == MTD_FD)
		return 0;
	return __real_close(fd);
}

off_t __real_lseek(int fd, off_t offset, int whence);
off_t __wrap_lseek(int fd, off_t offset, int whence);
off_t __wrap_lseek(int fd, off_t offset, int whence)
{
	if (fd == MTD_FD) {
		/* __swupdate_copy() executes lseek() on /dev/mtdX. */
		assert_int_equal(image.seek, offset);
		assert_int_equal(SEEK_SET, whence);
		return offset;
	}
	return __real_lseek(fd, offset, whence);
}

int __wrap_mtd_dev_present(libmtd_t desc, int mtd_num);
int __wrap_mtd_dev_present(libmtd_t UNUSED desc, int mtd_num)
{
	assert_int_equal(MTD_DEV_IDX, mtd_num);
	return 1;
}

int __wrap_get_mtd_from_device(char *s);
int __wrap_get_mtd_from_device(char *s)
{
	int ret, mtdnum;
	assert_ptr_not_equal(NULL, s);
	if (strlen(s) < sizeof(DEV_MTD_PREFIX))
		return -1;
	ret = sscanf(s, DEV_MTD_PREFIX "%d", &mtdnum);
	if (ret <= 0)
		return -1;
	return mtdnum;
}

static unsigned char get_byte_idx(int bit_idx)
{
	return bit_idx / CHAR_BIT;
}

static unsigned char get_mask(int bit_idx)
{
	return 1 << (bit_idx % CHAR_BIT);
}

static bool get_bit(unsigned char *data, int bit_idx)
{
	return (data[get_byte_idx(bit_idx)] & get_mask(bit_idx)) != 0;
}

static void set_bit(unsigned char *data, int bit_idx)
{
	data[get_byte_idx(bit_idx)] |= get_mask(bit_idx);
}

static void clear_bit(unsigned char *data, int bit_idx)
{
	data[get_byte_idx(bit_idx)] &= (~get_mask(bit_idx));
}

static void set_multiple_bits(unsigned char *data, const int *bit_indices)
{
	int bit_idx = bit_indices[0];
	for (int i = 0; bit_idx >= 0; bit_idx = bit_indices[++i])
		set_bit(data, bit_idx);
}

static void clear_multiple_bits(unsigned char *data, const int *bit_indices)
{
	int bit_idx = bit_indices[0];
	for (int i = 0; bit_idx >= 0; bit_idx = bit_indices[++i])
		clear_bit(data, bit_idx);
}

static void check_args(const struct mtd_dev_info *mtd, int fd, int eb)
{
	assert_ptr_equal(&mtd_ubi_info_s.mtd, mtd);
	assert_true(
	            (mtd->type == MTD_NORFLASH) ||
	            (mtd->type == MTD_NANDFLASH));
	assert_int_equal(MTD_FD, fd);
	assert_true(eb >= 0);
	assert_true(eb < mtd->size / mtd->eb_size);
}

static int default_mtd_get_bool(const struct mtd_dev_info UNUSED *mtd,
		int UNUSED fd, int eb, unsigned char *data)
{
	return get_bit(data, eb) ? 1 : 0;
}

static int default_mtd_is_bad(const struct mtd_dev_info *mtd, int fd, int eb)
{
	return default_mtd_get_bool(mtd, fd, eb, bad_blocks);
}
static int (*impl_mtd_is_bad)(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_is_bad(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_is_bad(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	return impl_mtd_is_bad(mtd, fd, eb);
}

static int default_mtd_mark_bad(const struct mtd_dev_info UNUSED *mtd,
		int UNUSED fd, int eb)
{
	set_bit(bad_blocks, eb);
	return 0;
}
static int (*impl_mtd_mark_bad)(const struct mtd_dev_info *mtd, int fd,
		int eb);
int __wrap_mtd_mark_bad(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_mark_bad(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	assert_false(get_bit(bad_blocks, eb));
	/* Note: we don't require erasing a block before marking it bad. */
	return impl_mtd_mark_bad(mtd, fd, eb);
}

static int default_mtd_is_locked(const struct mtd_dev_info *mtd, int fd,
		int eb)
{
	return default_mtd_get_bool(mtd, fd, eb, locked_blocks);
}
static int (*impl_mtd_is_locked)(const struct mtd_dev_info *mtd, int fd,
		int eb);
int __wrap_mtd_is_locked(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_is_locked(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	return impl_mtd_is_locked(mtd, fd, eb);
}

static int default_mtd_unlock(const struct mtd_dev_info UNUSED *mtd,
		int UNUSED fd, int eb)
{
	clear_bit(locked_blocks, eb);
	return 0;
}
static int (*impl_mtd_unlock)(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_unlock(const struct mtd_dev_info *mtd, int fd, int eb);
int __wrap_mtd_unlock(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	assert_false(get_bit(bad_blocks, eb));
	/* Unlocking already unlocked blocks is totaly fine:
	 * - not every flash supports mtd_is_locked() check;
	 * - some implementation might prefer to mtd_unlock() rigth away
	 *   without checking mtd_is_locked(). */
	return impl_mtd_unlock(mtd, fd, eb);
}

static int default_mtd_erase(libmtd_t UNUSED desc,
		const struct mtd_dev_info *mtd, int UNUSED fd, int eb)
{
	unsigned int page;
	memset(flash_memory + eb * mtd->eb_size, FLASH_EMPTY_BYTE,
	       mtd->eb_size);
	FOREACH_PAGE_IN_EB(page, mtd, eb)
		clear_bit(written_pages, page);
	return 0;
}
static int (*impl_mtd_erase)(libmtd_t desc, const struct mtd_dev_info *mtd,
		int fd, int eb);
int __wrap_mtd_erase(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		int eb);
int __wrap_mtd_erase(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		int eb)
{
	check_args(mtd, fd, eb);
	assert_false(get_bit(bad_blocks, eb));
	assert_false(get_bit(locked_blocks, eb));
	return impl_mtd_erase(desc, mtd, fd, eb);
}

static int default_mtd_write(libmtd_t UNUSED desc,
		const struct mtd_dev_info *mtd, int UNUSED fd, int eb,
		int offs, void *data, int len, void UNUSED *oob,
		int UNUSED ooblen, uint8_t UNUSED mode)
{
	unsigned int page;
	unsigned int flash_offset = eb * mtd->eb_size + offs;
	const unsigned char *pdata = (const unsigned char*)data;
	FOREACH_PAGE_WRITTEN(page, mtd, eb, offs, len) {
		set_bit(written_pages, page);
		memcpy(flash_memory + flash_offset, pdata, mtd->min_io_size);
		flash_offset += mtd->min_io_size;
		pdata += mtd->min_io_size;
	}
	return 0;
}

static int (*impl_mtd_write)(libmtd_t desc, const struct mtd_dev_info *mtd,
		int fd, int eb, int offs, void *data, int len, void *oob,
		int ooblen, uint8_t mode);
int __wrap_mtd_write(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		int eb, int offs, void *data, int len, void *oob, int ooblen,
		uint8_t mode);

int __wrap_mtd_write(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		int eb, int offs, void *data, int len, void *oob, int ooblen,
		uint8_t mode)
{
	unsigned int page;
	check_args(mtd, fd, eb);
	assert_true(offs >= 0);
	assert_ptr_not_equal(NULL, data);
	assert_true(len > 0);
	assert_true(offs + len <= mtd->eb_size);
	assert_ptr_equal(NULL, oob);
	assert_int_equal(0, ooblen);
	assert_false(get_bit(bad_blocks, eb));
	assert_false(get_bit(locked_blocks, eb));
	assert_true(offs <= mtd->eb_size - mtd->min_io_size);
	assert_int_equal(0, offs % mtd->min_io_size);
	assert_int_equal(0, len % mtd->min_io_size);

	if (mtd->type == MTD_NANDFLASH) {
		/* Follow "write once rule" for NAND flash. */
		FOREACH_PAGE_WRITTEN(page, mtd, eb, offs, len)
			assert_false(get_bit(written_pages, page));
	} else {
		/* Assume it is ok to write multiple times to NOR flash. */
		unsigned int flash_i = eb * mtd->eb_size + offs;
		for (int data_i = 0; data_i < len; data_i++) {
			uint8_t old_byte = flash_memory[flash_i + data_i];
			uint8_t new_byte = ((uint8_t*)data)[data_i];
			assert_true((old_byte & new_byte) == new_byte);
		}
	}
	return impl_mtd_write(desc, mtd, fd, eb, offs, data, len, oob, ooblen,
	                      mode);
}

static int default_mtd_read(const struct mtd_dev_info *mtd, int fd, int eb,
		int offs, void *buf, int len)
{
	unsigned int flash_offset = eb * mtd->eb_size + offs;
	check_args(mtd, fd, eb);
	memcpy(buf, flash_memory + flash_offset, len);
	return 0;
}
static int (*impl_mtd_read)(const struct mtd_dev_info *mtd, int fd, int eb,
		int offs, void *buf, int len);
int __wrap_mtd_read(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
		void *buf, int len);
int __wrap_mtd_read(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
		void *buf, int len)
{
	check_args(mtd, fd, eb);
	assert_true(offs >= 0);
	assert_ptr_not_equal(NULL, buf);
	assert_true(len > 0);
	assert_true(offs + len <= mtd->eb_size);
	assert_false(get_bit(bad_blocks, eb));
	assert_false(get_bit(locked_blocks, eb));
	assert_true(offs <= mtd->eb_size - mtd->min_io_size);
	assert_int_equal(0, offs % mtd->min_io_size);
	assert_int_equal(0, len % mtd->min_io_size);
	return impl_mtd_read(mtd, fd, eb, offs, buf, len);
}

static unsigned char *generate_image_file(const char *name, size_t size)
{
	int r, fd;
	unsigned char *buf = malloc(size);
	unsigned char *pbuf = buf;
	assert_ptr_not_equal(NULL, buf);
	fd = open(name, O_WRONLY);
	if (fd < 0)
		free(buf);
	assert_true(fd >= 0);

	for (size_t i = 0; i < size; i++)
		buf[i] = (unsigned char)i;

	if (patch_image_buf)
		patch_image_buf(buf);

	while (size > 0) {
		ssize_t ret = write(fd, pbuf, size);
		if (ret < 0) {
			close(fd);
			free(buf);
		}
		assert_true(ret >= 0);
		pbuf += ret;
		size -= ret;
	}

	r = close(fd);
	assert_int_equal(0, r);
	return buf;
}

static int group_setup(void UNUSED **state)
{
	int ret, fd;
	struct flash_description *flash = get_flash_info();
	struct installer_handler *hnd = find_handler(&image);

	assert_ptr_not_equal(NULL, hnd);
	handler_func = hnd->installer;
	assert_ptr_not_equal(NULL, handler_func);
	flash->mtd_info = &mtd_ubi_info_s;
	flash->libmtd = LIBMTD_T_VALUE;

#ifdef PRINT_LOGS
	loglevel = DEBUGLEVEL;
	notify_init();
#endif

	strcpy(image.fname, "swupdate_image_XXXXXX.bin");
	fd = mkstemps(image.fname, 4 /* = strlen(".bin") */);
	if (fd < 0)
		image.fname[0] = 0; /* required for group_teardown() */
	ret = close(fd);
	assert_int_equal(0, ret);
	return 0;
}

static int group_teardown(void UNUSED **state)
{
	if (image.fname[0] != 0) {
		int ret = unlink(image.fname);
		assert_int_equal(0, ret);
	}
	return 0;
}

static void copy_flash_state(void)
{
	struct mtd_dev_info *mtd = &mtd_ubi_info_s.mtd;

	expected_bad_blocks = malloc(eb_bytes);
	assert_ptr_not_equal(NULL, expected_bad_blocks);
	memcpy(expected_bad_blocks, bad_blocks, eb_bytes);

	expected_locked_blocks = malloc(eb_bytes);
	assert_ptr_not_equal(NULL, expected_locked_blocks);
	memcpy(expected_locked_blocks, locked_blocks, eb_bytes);

	expected_written_pages = malloc(pages_bytes);
	assert_ptr_not_equal(NULL, expected_written_pages);
	memcpy(expected_written_pages, written_pages, pages_bytes);

	expected_flash_memory = malloc(mtd->size);
	assert_ptr_not_equal(NULL, expected_flash_memory);
	memcpy(expected_flash_memory, flash_memory, mtd->size);
}

static void init_flash_state(void)
{
	struct mtd_dev_info *mtd = &mtd_ubi_info_s.mtd;
	int num = (int)(mtd->size / mtd->eb_size);
	eb_bytes = num / CHAR_BIT;
	if (num % CHAR_BIT)
		eb_bytes++;
	/* By default there are no any bad blocks: */
	bad_blocks = calloc(eb_bytes, 1);
	assert_ptr_not_equal(NULL, bad_blocks);
	/* By default all blocks are locked: */
	locked_blocks = malloc(eb_bytes);
	assert_ptr_not_equal(NULL, locked_blocks);
	memset(locked_blocks, 0xFF, eb_bytes);

	num = (int)(mtd->size / mtd->min_io_size);
	pages_bytes = num / CHAR_BIT;
	if (num % CHAR_BIT)
		pages_bytes++;
	/* By default all pages are written (require erase): */
	written_pages = malloc(pages_bytes);
	assert_ptr_not_equal(NULL, written_pages);
	memset(written_pages, 0xFF, pages_bytes);

	flash_memory = malloc(mtd->size);
	assert_ptr_not_equal(NULL, flash_memory);
	/* Fill flash memory with initial pattern: */
	memset(flash_memory, 0xA5, mtd->size);
}

static void test_init(void)
{
	struct mtd_dev_info *mtd = &mtd_ubi_info_s.mtd;

	/* Unit test config (precondition) sanity check: */
	assert_true(mtd->size >= mtd->eb_size);
	assert_true(mtd->eb_size >= mtd->min_io_size);
	assert_int_equal(0, mtd->size % mtd->eb_size);
	assert_int_equal(0, mtd->eb_size % mtd->min_io_size);

	is_nand = (mtd->type == MTD_NANDFLASH) ||
	          (mtd->type == MTD_MLCNANDFLASH);
	image_buf = generate_image_file(image.fname, image.size);
	image.fdin = open(image.fname, O_RDONLY);
	assert_true(image.fdin >= 0);

	init_flash_state();
}

static void verify_flash_state(void)
{
	struct mtd_dev_info *mtd = &mtd_ubi_info_s.mtd;
	assert_memory_equal(expected_bad_blocks, bad_blocks, eb_bytes);
	assert_memory_equal(expected_locked_blocks, locked_blocks, eb_bytes);
	assert_memory_equal(expected_written_pages, written_pages,
	                    pages_bytes);
	assert_memory_equal(expected_flash_memory, flash_memory, mtd->size);
}

static void run_flash_test(void UNUSED **state, int expected_return_code)
{
	int ret = handler_func(&image, NULL);
#ifdef RETURN_CODE_ENOSPC
	/* Currently on callback() failure __swupdate_copy() always returns
	 * -ENOSPC (no matter what callback() returned). Hence some subset of
	 * flash handler return values are converted to -ENOSPC. Here we'll
	 * convert rest of return values to -ENOSPC to map expected and actual
	 * return values to a simple success/failure check. */
	if (ret < 0)
		ret = -ENOSPC;
	if (expected_return_code < 0)
		expected_return_code = -ENOSPC;
#endif
	assert_int_equal(expected_return_code, ret);
	verify_flash_state();
}

static int test_setup(void UNUSED **state)
{
	struct mtd_dev_info *mtd = &mtd_ubi_info_s.mtd;
	image_buf = NULL;
	patch_image_buf = NULL;
	image.fdin = -1;

	bad_blocks = NULL;
	locked_blocks = NULL;
	written_pages = NULL;
	flash_memory = NULL;

	expected_bad_blocks = NULL;
	expected_locked_blocks = NULL;
	expected_written_pages = NULL;
	expected_flash_memory = NULL;

	/* Tests can overwrite the following default implementations to inject
	 * errors: */
	impl_malloc = __real_malloc;
	impl_open = default_open;
	impl_mtd_is_bad = default_mtd_is_bad;
	impl_mtd_mark_bad = default_mtd_mark_bad;
	impl_mtd_is_locked = default_mtd_is_locked;
	impl_mtd_unlock = default_mtd_unlock;
	impl_mtd_erase = default_mtd_erase;
	impl_mtd_write = default_mtd_write;
	impl_mtd_read = default_mtd_read;

	/* Some default values: */
	mtd->type = MTD_NANDFLASH;
	strcpy(image.device, DEV_MTD_PREFIX STR(MTD_DEV_IDX));
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	return 0;
}

static int test_teardown(void UNUSED **state)
{
	unsigned char *pointer[] = {
		bad_blocks,
		locked_blocks,
		written_pages,
		flash_memory,

		expected_bad_blocks,
		expected_locked_blocks,
		expected_written_pages,
		expected_flash_memory,

		image_buf,
	};
	impl_malloc = __real_malloc;
	for (int i = 0; i < ARRAY_SIZE(pointer); i++)
		free(pointer[i]);
	if (image.fdin >= 0) {
		int ret = close(image.fdin);
		assert_int_equal(0, ret);
	}
	return 0;
}

static void test_simple(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_simple_NOR(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 1;
	mtd_ubi_info_s.mtd.type = MTD_NORFLASH;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_padding_less_than_page(void **state)
{
	image.seek = 0;
	image.size = 42;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	memset(expected_flash_memory + 42, FLASH_EMPTY_BYTE, 6);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_padding_page(void **state)
{
	image.seek = 0;
	image.size = 40;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	memset(expected_flash_memory + 40, FLASH_EMPTY_BYTE, 8);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 5);

	run_flash_test(state, 0);
}

static void patch_image_buf_empty_bytes_1(unsigned char *buf)
{
	memset(buf + 8, FLASH_EMPTY_BYTE, 24);
}

static void test_skip_write_page_empty_bytes(void **state)
{
	image.seek = 0;
	image.size = 40;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	patch_image_buf = patch_image_buf_empty_bytes_1;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	memset(expected_flash_memory + 40, FLASH_EMPTY_BYTE, 8);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);
	if (is_nand)
		clear_bit(expected_written_pages, 1);
	clear_bit(expected_written_pages, 2);
	clear_bit(expected_written_pages, 3);
	clear_bit(expected_written_pages, 5);

	run_flash_test(state, 0);
}

static void test_padding_more_than_page(void **state)
{
	image.seek = 0;
	image.size = 37;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	memset(expected_flash_memory + 37, FLASH_EMPTY_BYTE, 11);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 5);

	run_flash_test(state, 0);
}

static void test_seek(void **state)
{
	image.seek = 16;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory + 16, image_buf, image.size);
	for (int i = 1; i <= 3; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_seek_not_multiple_of_eb_size(void **state)
{
	image.seek = 8;
	mtd_ubi_info_s.mtd.eb_size = 16;
	test_init();
	copy_flash_state();
	run_flash_test(state, -EINVAL);
}

static void test_not_enough_flash(void **state)
{
	image.seek = 16;
	image.size = 1020;
	mtd_ubi_info_s.mtd.size = 1024;
	test_init();
	copy_flash_state();
	run_flash_test(state, -ENOSPC);
}

static void test_invalid_mtd_device(void **state)
{
	test_init();
	strcpy(image.device, "/dev/mtdX");
	copy_flash_state();
	run_flash_test(state, -EINVAL);
}

static void test_invalid_image_size(void **state)
{
	test_init();
	image.size = -42;
	copy_flash_state();
	run_flash_test(state, -42);
}

static void test_empty_image(void **state)
{
	test_init();
	image.size = 0;
	copy_flash_state();
	run_flash_test(state, 0);
}

static int open_mtd_dev_failure_errno;
static int open_mtd_dev_failure(const char *pathname, int flags)
{
	if (strcmp(pathname, image.device) == 0) {
		errno = open_mtd_dev_failure_errno;
		return -1;
	}
	return default_open(pathname, flags);
}
static void test_mtd_dev_open_failure(void **state)
{
	test_init();
	impl_open = open_mtd_dev_failure;
	open_mtd_dev_failure_errno = EPERM;
	copy_flash_state();
	run_flash_test(state, -open_mtd_dev_failure_errno);
}

static int malloc_filebuf_allocation_failure_size;
static void *malloc_filebuf_allocation_failure(size_t size)
{
	if (size == malloc_filebuf_allocation_failure_size) {
		errno = ENOMEM;
		return NULL;
	}
	return __real_malloc(size);
}
static void test_malloc_failure(void **state)
{
	test_init();
	copy_flash_state();
	impl_malloc = malloc_filebuf_allocation_failure;
	malloc_filebuf_allocation_failure_size = mtd_ubi_info_s.mtd.eb_size;
	if (!is_nand) {
		/* filebuf allocation includes space for readout buffer */
		malloc_filebuf_allocation_failure_size *= 2;
	}
	run_flash_test(state, -ENOMEM);
}

static void test_skip_bad_blocks(void **state)
{
	image.seek = 896;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	/* Initial flash state:                    | flash memory ends here
	 * eb      56  57  58  59  60  61  62   63 |   64
	 * offset 896 912 928 944 960 976 992 1008 | 1024
	 * status bad  ok bad  ok bad bad  ok  bad | */

	{
		int indices[] = {56, 58, 60, 61, 63, -1};
		set_multiple_bits(bad_blocks, indices);
	}
	copy_flash_state();

	{
		int indices[] = {57, 59, 62, -1};
		clear_multiple_bits(expected_locked_blocks, indices);
	}
	memcpy(expected_flash_memory + 912, image_buf     , 16);
	memcpy(expected_flash_memory + 944, image_buf + 16, 16);
	memcpy(expected_flash_memory + 992, image_buf + 32, 16);

	run_flash_test(state, 0);
}

static void test_too_many_known_bad_blocks(void **state)
{
	image.seek = 896;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	/* Initial flash state:                    | flash memory ends here
	 * eb      56  57  58  59  60  61  62   63 |   64
	 * offset 896 912 928 944 960 976 992 1008 | 1024
	 * status bad  ok bad  ok bad bad bad  bad | */

	{
		int indices[] = {56, 58, 60, 61, 62, 63, -1};
		set_multiple_bits(bad_blocks, indices);
	}
	copy_flash_state();

	{
		int indices[] = {57, 59, -1};
		clear_multiple_bits(expected_locked_blocks, indices);
	}
	memcpy(expected_flash_memory + 912, image_buf     , 16);
	memcpy(expected_flash_memory + 944, image_buf + 16, 16);

	run_flash_test(state, -ENOSPC);
}

static int mtd_write_failure_1_eb;
static int mtd_write_failure_1_offs;
static int mtd_write_failure_1_errno;
static int mtd_write_failure_1(libmtd_t desc, const struct mtd_dev_info *mtd,
		int fd, int eb, int offs, void *data, int len, void *oob,
		int ooblen, uint8_t mode)
{
	if (eb == mtd_write_failure_1_eb) {
		unsigned char *p_data = (unsigned char *)data;
		for (int o = offs; o < offs + len; o += mtd->min_io_size) {
			int ret;
			if (o == mtd_write_failure_1_offs) {
				errno = mtd_write_failure_1_errno;
				return -1;
			}
			ret = default_mtd_write(desc, mtd, fd, eb, o,
			                        p_data, mtd->min_io_size, oob,
			                        ooblen, mode);
			assert_int_equal(0, ret);
			p_data += mtd->min_io_size;
		}
	}
	return default_mtd_write(desc, mtd, fd, eb, offs, data, len, oob,
	                         ooblen, mode);
}

static void test_too_many_unknown_bad_blocks(void **state)
{
	image.seek = 896;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_offs = 0;
	mtd_write_failure_1_errno = EIO;

	/* Initial flash state:                       | flash memory ends here
	 * eb      56  57  58  59  60  61  62      63 |   64
	 * offset 896 912 928 944 960 976 992    1008 | 1024
	 * status bad  ok bad  ok bad bad bad ok->bad | */
	{
		int indices[] = {56, 58, 60, 61, 62, -1};
		set_multiple_bits(bad_blocks, indices);
	}
	mtd_write_failure_1_eb = 63;
	copy_flash_state();

	{
		int indices[] = {57, 59, 63, -1};
		clear_multiple_bits(expected_locked_blocks, indices);
	}
	memcpy(expected_flash_memory +  912, image_buf     , 16);
	memcpy(expected_flash_memory +  944, image_buf + 16, 16);
	memset(expected_flash_memory + 1008, FLASH_EMPTY_BYTE, 16);
	set_bit(expected_bad_blocks, 63);
	clear_bit(expected_written_pages, 63 * 2);
	clear_bit(expected_written_pages, 63 * 2 + 1);

	run_flash_test(state, -ENOSPC);
}

static int mtd_is_bad_failure_1_errno;
static int mtd_is_bad_failure_1(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	errno = mtd_is_bad_failure_1_errno;
	return -1;
}

static void test_mtd_is_bad_not_supported(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_is_bad = mtd_is_bad_failure_1;
	mtd_is_bad_failure_1_errno = EOPNOTSUPP;

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_mtd_is_bad_failure(void **state)
{
	test_init();
	impl_mtd_is_bad = mtd_is_bad_failure_1;
	mtd_is_bad_failure_1_errno = ERANGE; /* dummy value */
	copy_flash_state();
	run_flash_test(state, -mtd_is_bad_failure_1_errno);
}

static int mtd_is_locked_failure_1_errno;
static int mtd_is_locked_failure_1(const struct mtd_dev_info *mtd, int fd,
		int eb)
{
	check_args(mtd, fd, eb);
	errno = mtd_is_locked_failure_1_errno;
	return -1;
}

static void test_mtd_is_locked_not_supported(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_is_locked = mtd_is_locked_failure_1;
	mtd_is_locked_failure_1_errno = EOPNOTSUPP;

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_mtd_is_locked_failure(void **state)
{
	test_init();
	impl_mtd_is_locked = mtd_is_locked_failure_1;
	mtd_is_locked_failure_1_errno = ERANGE; /* dummy value */
	copy_flash_state();
	run_flash_test(state, -mtd_is_locked_failure_1_errno);
}

static int mtd_unlock_failure_1_errno;
static int mtd_unlock_failure_1(const struct mtd_dev_info *mtd, int fd, int eb)
{
	check_args(mtd, fd, eb);
	errno = mtd_unlock_failure_1_errno;
	return -1;
}

static void test_mtd_unlock_not_supported(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_unlock = mtd_unlock_failure_1;
	mtd_unlock_failure_1_errno = EOPNOTSUPP;
	memset(locked_blocks, 0, eb_bytes);

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_mtd_unlock_failure(void **state)
{
	test_init();
	impl_mtd_unlock = mtd_unlock_failure_1;
	mtd_unlock_failure_1_errno = ERANGE; /* dummy value */
	copy_flash_state();
	run_flash_test(state, -mtd_unlock_failure_1_errno);
}

static int mtd_read_failure_1_errno;
static int mtd_read_failure_1(const struct mtd_dev_info *mtd, int fd, int eb,
		int UNUSED offs, void UNUSED *buf, int UNUSED len)
{
	check_args(mtd, fd, eb);
	errno = mtd_read_failure_1_errno;
	return -1;
}

static void test_mtd_read_failure(void **state)
{
	mtd_ubi_info_s.mtd.type = MTD_NORFLASH;
	test_init();
	impl_mtd_read = mtd_read_failure_1;
	mtd_read_failure_1_errno = ERANGE; /* dummy value */
	copy_flash_state();
	clear_bit(expected_locked_blocks, 0);
	run_flash_test(state, -mtd_read_failure_1_errno);
}

static int mtd_erase_failure_1_eb;
static int mtd_erase_failure_1_errno;
static int mtd_erase_failure_1(libmtd_t desc, const struct mtd_dev_info *mtd,
		int fd, int eb)
{
	if (eb == mtd_erase_failure_1_eb) {
		errno = mtd_erase_failure_1_errno;
		if (errno == EOPNOTSUPP)
			default_mtd_erase(desc, mtd, fd, eb);
		return -1;
	}
	return default_mtd_erase(desc, mtd, fd, eb);
}

static void test_mtd_read_no_erase_empty_flash_bytes(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.type = MTD_NORFLASH;
	test_init();
	memset(flash_memory, FLASH_EMPTY_BYTE, 16);
	impl_mtd_erase = mtd_erase_failure_1;
	mtd_erase_failure_1_eb = 0;
	mtd_erase_failure_1_errno = ERANGE; /* dummy value */

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_mtd_erase_not_supported(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_erase = mtd_erase_failure_1;
	mtd_erase_failure_1_eb = 0;
	mtd_erase_failure_1_errno = EOPNOTSUPP;

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static void test_mtd_erase_failure(void **state)
{
	test_init();
	impl_mtd_erase = mtd_erase_failure_1;
	mtd_erase_failure_1_eb = 0;
	mtd_erase_failure_1_errno = ERANGE; /* dummy value */
	copy_flash_state();
	clear_bit(expected_locked_blocks, 0);
	run_flash_test(state, -mtd_erase_failure_1_errno);
}

static void test_mtd_erase_failure_EIO(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_erase = mtd_erase_failure_1;
	mtd_erase_failure_1_eb = 0;
	mtd_erase_failure_1_errno = EIO;

	copy_flash_state();
	memcpy(expected_flash_memory + 16, image_buf, image.size);
	for (int i = 0; i <= 3; i++)
		clear_bit(expected_locked_blocks, i);
	set_bit(expected_bad_blocks, 0);

	run_flash_test(state, 0);
}

static void patch_image_buf_empty_bytes_2(unsigned char *buf)
{
	memset(buf, FLASH_EMPTY_BYTE, 16);
}

static void test_no_mtd_write_empty_image_bytes(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	patch_image_buf = patch_image_buf_empty_bytes_2;
	test_init();
	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 0;
	mtd_write_failure_1_offs = 0;
	mtd_write_failure_1_errno = ERANGE; /* dummy value */

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	for (int i = 0; i <= 2; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 0);
	clear_bit(expected_written_pages, 1);

	run_flash_test(state, 0);
}

static void test_mtd_write_failure(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = ERANGE; /* dummy value */

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, 24);
	memset(expected_flash_memory + 24, FLASH_EMPTY_BYTE, 8);
	for (int i = 0; i <= 1; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 3);

	run_flash_test(state, -mtd_write_failure_1_errno);
}

static void test_mtd_write_bad_block(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();
	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = EIO;

	copy_flash_state();
	set_bit(expected_bad_blocks, 1);
	clear_bit(expected_written_pages, 2);
	clear_bit(expected_written_pages, 3);
	memcpy(expected_flash_memory, image_buf, 16);
	memset(expected_flash_memory + 16, FLASH_EMPTY_BYTE, 16);
	memcpy(expected_flash_memory + 32, image_buf + 16, 32);
	for (int i = 0; i <= 3; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static int mtd_erase_failure_2_eb;
static int mtd_erase_failure_2_idx;
static int mtd_erase_failure_2_errno;
static int mtd_erase_failure_2(libmtd_t desc, const struct mtd_dev_info *mtd,
		int fd, int eb)
{
	check_args(mtd, fd, eb);
	if (eb == mtd_erase_failure_2_eb) {
		if (mtd_erase_failure_2_idx == 0) {
			errno = mtd_erase_failure_2_errno;
			return -1;
		}
		--mtd_erase_failure_2_idx;
	}
	return default_mtd_erase(desc, mtd, fd, eb);
}

static void test_mtd_write_bad_block_erase_failure(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = EIO;

	impl_mtd_erase = mtd_erase_failure_2;
	mtd_erase_failure_2_eb = 1;
	mtd_erase_failure_2_idx = 1;
	mtd_erase_failure_2_errno = ERANGE; /* dummy value */

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, 24);
	memset(expected_flash_memory + 24, FLASH_EMPTY_BYTE, 8);
	for (int i = 0; i <= 1; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 3);

	run_flash_test(state, -mtd_erase_failure_2_errno);
}

static void test_mtd_write_bad_block_erase_failure_EIO(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = EIO;

	impl_mtd_erase = mtd_erase_failure_2;
	mtd_erase_failure_2_eb = 1;
	mtd_erase_failure_2_idx = 1;
	mtd_erase_failure_2_errno = EIO;

	copy_flash_state();
	set_bit(expected_bad_blocks, 1);
	clear_bit(expected_written_pages, 3);
	memcpy(expected_flash_memory, image_buf, 24);
	memset(expected_flash_memory + 24, FLASH_EMPTY_BYTE, 8);
	memcpy(expected_flash_memory + 32, image_buf + 16, 32);
	for (int i = 0; i <= 3; i++)
		clear_bit(expected_locked_blocks, i);

	run_flash_test(state, 0);
}

static int mtd_mark_bad_failure_1_errno;
static int mtd_mark_bad_failure_1(const struct mtd_dev_info *mtd, int fd,
		int eb)
{
	check_args(mtd, fd, eb);
	errno = mtd_mark_bad_failure_1_errno;
	return -1;
}

static void test_mtd_write_bad_block_mark_not_supported(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = EIO;

	impl_mtd_mark_bad = mtd_mark_bad_failure_1;
	mtd_mark_bad_failure_1_errno = EOPNOTSUPP;

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, 16);
	memset(expected_flash_memory + 16, FLASH_EMPTY_BYTE, 16);
	memcpy(expected_flash_memory + 32, image_buf + 16, 32);

	for (int i = 0; i <= 3; i++)
		clear_bit(expected_locked_blocks, i);
	for (int i = 2; i <= 3; i++)
		clear_bit(expected_written_pages, i);

	run_flash_test(state, 0);
}

static void test_mtd_write_bad_block_mark_failure(void **state)
{
	image.seek = 0;
	image.size = 48;
	mtd_ubi_info_s.mtd.size = 1024;
	mtd_ubi_info_s.mtd.eb_size = 16;
	mtd_ubi_info_s.mtd.min_io_size = 8;
	test_init();

	impl_mtd_write = mtd_write_failure_1;
	mtd_write_failure_1_eb = 1;
	mtd_write_failure_1_offs = 8;
	mtd_write_failure_1_errno = EIO;

	impl_mtd_mark_bad = mtd_mark_bad_failure_1;
	mtd_mark_bad_failure_1_errno = ERANGE; /* dummy value */

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, 16);
	memset(expected_flash_memory + 16, FLASH_EMPTY_BYTE, 16);
	for (int i = 0; i <= 1; i++)
		clear_bit(expected_locked_blocks, i);
	for (int i = 2; i <= 3; i++)
		clear_bit(expected_written_pages, i);

	run_flash_test(state, -mtd_mark_bad_failure_1_errno);
}

static void test_multiple_callbacks(void **state)
{
	/* flash_write() is typically executed with (len <= 16 * 1024) */
	image.seek = 0;
	image.size = 63 * 1024;
	mtd_ubi_info_s.mtd.size = 64 * 1024;
	mtd_ubi_info_s.mtd.eb_size = 8 * 1024;
	mtd_ubi_info_s.mtd.min_io_size = 1024;
	test_init();

	copy_flash_state();
	memcpy(expected_flash_memory, image_buf, image.size);
	memset(expected_flash_memory + 63 * 1024, FLASH_EMPTY_BYTE, 1024);
	for (int i = 0; i <= 7; i++)
		clear_bit(expected_locked_blocks, i);
	clear_bit(expected_written_pages, 63);

	run_flash_test(state, 0);
}

#define TEST(name) \
	cmocka_unit_test_setup_teardown(test_##name, test_setup, test_teardown)

int main(void)
{
	static const struct CMUnitTest tests[] = {
		TEST(simple),
		TEST(simple_NOR),
		TEST(padding_less_than_page),
		TEST(padding_page),
		TEST(skip_write_page_empty_bytes),
		TEST(padding_more_than_page),
		TEST(seek),
		TEST(seek_not_multiple_of_eb_size),
		TEST(not_enough_flash),
		TEST(invalid_mtd_device),
		TEST(invalid_image_size),
		TEST(empty_image),
		TEST(mtd_dev_open_failure),
		TEST(malloc_failure),
		TEST(skip_bad_blocks),
		TEST(too_many_known_bad_blocks),
		TEST(too_many_unknown_bad_blocks),
		TEST(mtd_is_bad_not_supported),
		TEST(mtd_is_bad_failure),
		TEST(mtd_is_locked_not_supported),
		TEST(mtd_is_locked_failure),
		TEST(mtd_unlock_not_supported),
		TEST(mtd_unlock_failure),
		TEST(mtd_read_failure),
		TEST(mtd_read_no_erase_empty_flash_bytes),
		TEST(mtd_erase_not_supported),
		TEST(mtd_erase_failure),
		TEST(mtd_erase_failure_EIO),
		TEST(no_mtd_write_empty_image_bytes),
		TEST(mtd_write_failure),
		TEST(mtd_write_bad_block),
		TEST(mtd_write_bad_block_erase_failure),
		TEST(mtd_write_bad_block_erase_failure_EIO),
		TEST(mtd_write_bad_block_mark_not_supported),
		TEST(mtd_write_bad_block_mark_failure),
		TEST(multiple_callbacks),
	};
	return cmocka_run_group_tests_name("flash_handler", tests, group_setup,
	                                   group_teardown);
}
