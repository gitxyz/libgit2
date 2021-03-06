/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "common.h"
#include "git2/blob.h"
#include "git2/submodule.h"
#include "diff.h"
#include "diff_file.h"
#include "odb.h"
#include "fileops.h"
#include "filter.h"

#define DIFF_MAX_FILESIZE 0x20000000

static bool diff_file_content_binary_by_size(git_diff_file_content *fc)
{
	/* if we have diff opts, check max_size vs file size */
	if ((fc->file.flags & DIFF_FLAGS_KNOWN_BINARY) == 0 &&
		fc->opts_max_size > 0 &&
		fc->file.size > fc->opts_max_size)
		fc->file.flags |= GIT_DIFF_FLAG_BINARY;

	return ((fc->file.flags & GIT_DIFF_FLAG_BINARY) != 0);
}

static void diff_file_content_binary_by_content(git_diff_file_content *fc)
{
	if ((fc->file.flags & DIFF_FLAGS_KNOWN_BINARY) != 0)
		return;

	switch (git_diff_driver_content_is_binary(
		fc->driver, fc->map.data, fc->map.len)) {
	case 0: fc->file.flags |= GIT_DIFF_FLAG_NOT_BINARY; break;
	case 1: fc->file.flags |= GIT_DIFF_FLAG_BINARY; break;
	default: break;
	}
}

static int diff_file_content_init_common(
	git_diff_file_content *fc, const git_diff_options *opts)
{
	fc->opts_flags = opts ? opts->flags : GIT_DIFF_NORMAL;

	if (opts && opts->max_size >= 0)
		fc->opts_max_size = opts->max_size ?
			opts->max_size : DIFF_MAX_FILESIZE;

	if (!fc->driver) {
		if (git_diff_driver_lookup(&fc->driver, fc->repo, "") < 0)
			return -1;
		fc->src = GIT_ITERATOR_TYPE_TREE;
	}

	/* give driver a chance to modify options */
	git_diff_driver_update_options(&fc->opts_flags, fc->driver);

	/* make sure file is conceivable mmap-able */
	if ((git_off_t)((size_t)fc->file.size) != fc->file.size)
		fc->file.flags |= GIT_DIFF_FLAG_BINARY;
	/* check if user is forcing text diff the file */
	else if (fc->opts_flags & GIT_DIFF_FORCE_TEXT) {
		fc->file.flags &= ~GIT_DIFF_FLAG_BINARY;
		fc->file.flags |= GIT_DIFF_FLAG_NOT_BINARY;
	}
	/* check if user is forcing binary diff the file */
	else if (fc->opts_flags & GIT_DIFF_FORCE_BINARY) {
		fc->file.flags &= ~GIT_DIFF_FLAG_NOT_BINARY;
		fc->file.flags |= GIT_DIFF_FLAG_BINARY;
	}

	diff_file_content_binary_by_size(fc);

	if ((fc->file.flags & GIT_DIFF_FLAG__NO_DATA) != 0) {
		fc->file.flags |= GIT_DIFF_FLAG__LOADED;
		fc->map.len  = 0;
		fc->map.data = "";
	}

	if ((fc->file.flags & GIT_DIFF_FLAG__LOADED) != 0)
		diff_file_content_binary_by_content(fc);

	return 0;
}

int git_diff_file_content__init_from_diff(
	git_diff_file_content *fc,
	git_diff_list *diff,
	size_t delta_index,
	bool use_old)
{
	git_diff_delta *delta = git_vector_get(&diff->deltas, delta_index);
	git_diff_file *file = use_old ? &delta->old_file : &delta->new_file;
	bool has_data = true;

	memset(fc, 0, sizeof(*fc));
	fc->repo = diff->repo;
	fc->src  = use_old ? diff->old_src : diff->new_src;
	memcpy(&fc->file, file, sizeof(fc->file));

	if (git_diff_driver_lookup(&fc->driver, fc->repo, file->path) < 0)
		return -1;

	switch (delta->status) {
	case GIT_DELTA_ADDED:
		has_data = !use_old; break;
	case GIT_DELTA_DELETED:
		has_data = use_old; break;
	case GIT_DELTA_UNTRACKED:
		has_data = !use_old &&
			(diff->opts.flags & GIT_DIFF_INCLUDE_UNTRACKED_CONTENT) != 0;
		break;
	case GIT_DELTA_MODIFIED:
	case GIT_DELTA_COPIED:
	case GIT_DELTA_RENAMED:
		break;
	default:
		has_data = false;
		break;
	}

	if (!has_data)
		fc->file.flags |= GIT_DIFF_FLAG__NO_DATA;

	return diff_file_content_init_common(fc, &diff->opts);
}

int git_diff_file_content__init_from_blob(
	git_diff_file_content *fc,
	git_repository *repo,
	const git_diff_options *opts,
	const git_blob *blob)
{
	memset(fc, 0, sizeof(*fc));
	fc->repo = repo;
	fc->blob = blob;

	if (!blob) {
		fc->file.flags |= GIT_DIFF_FLAG__NO_DATA;
	} else {
		fc->file.flags |= GIT_DIFF_FLAG__LOADED | GIT_DIFF_FLAG_VALID_OID;
		fc->file.size = git_blob_rawsize(blob);
		fc->file.mode = 0644;
		git_oid_cpy(&fc->file.oid, git_blob_id(blob));

		fc->map.len = (size_t)fc->file.size;
		fc->map.data = (char *)git_blob_rawcontent(blob);
	}

	return diff_file_content_init_common(fc, opts);
}

int git_diff_file_content__init_from_raw(
	git_diff_file_content *fc,
	git_repository *repo,
	const git_diff_options *opts,
	const char *buf,
	size_t buflen)
{
	memset(fc, 0, sizeof(*fc));
	fc->repo = repo;

	if (!buf) {
		fc->file.flags |= GIT_DIFF_FLAG__NO_DATA;
	} else {
		fc->file.flags |= GIT_DIFF_FLAG__LOADED | GIT_DIFF_FLAG_VALID_OID;
		fc->file.size = buflen;
		fc->file.mode = 0644;
		git_odb_hash(&fc->file.oid, buf, buflen, GIT_OBJ_BLOB);

		fc->map.len  = buflen;
		fc->map.data = (char *)buf;
	}

	return diff_file_content_init_common(fc, opts);
}

static int diff_file_content_commit_to_str(
	git_diff_file_content *fc, bool check_status)
{
	char oid[GIT_OID_HEXSZ+1];
	git_buf content = GIT_BUF_INIT;
	const char *status = "";

	if (check_status) {
		int error = 0;
		git_submodule *sm = NULL;
		unsigned int sm_status = 0;
		const git_oid *sm_head;

		if ((error = git_submodule_lookup(&sm, fc->repo, fc->file.path)) < 0 ||
			(error = git_submodule_status(&sm_status, sm)) < 0) {
			/* GIT_EEXISTS means a "submodule" that has not been git added */
			if (error == GIT_EEXISTS)
				error = 0;
			return error;
		}

		/* update OID if we didn't have it previously */
		if ((fc->file.flags & GIT_DIFF_FLAG_VALID_OID) == 0 &&
			((sm_head = git_submodule_wd_id(sm)) != NULL ||
			 (sm_head = git_submodule_head_id(sm)) != NULL))
		{
			git_oid_cpy(&fc->file.oid, sm_head);
			fc->file.flags |= GIT_DIFF_FLAG_VALID_OID;
		}

		if (GIT_SUBMODULE_STATUS_IS_WD_DIRTY(sm_status))
			status = "-dirty";
	}

	git_oid_tostr(oid, sizeof(oid), &fc->file.oid);
	if (git_buf_printf(&content, "Subproject commit %s%s\n", oid, status) < 0)
		return -1;

	fc->map.len  = git_buf_len(&content);
	fc->map.data = git_buf_detach(&content);
	fc->file.flags |= GIT_DIFF_FLAG__FREE_DATA;

	return 0;
}

static int diff_file_content_load_blob(git_diff_file_content *fc)
{
	int error = 0;
	git_odb_object *odb_obj = NULL;

	if (git_oid_iszero(&fc->file.oid))
		return 0;

	if (fc->file.mode == GIT_FILEMODE_COMMIT)
		return diff_file_content_commit_to_str(fc, false);

	/* if we don't know size, try to peek at object header first */
	if (!fc->file.size) {
		git_odb *odb;
		size_t len;
		git_otype type;

		if (!(error = git_repository_odb__weakptr(&odb, fc->repo))) {
			error = git_odb__read_header_or_object(
				&odb_obj, &len, &type, odb, &fc->file.oid);
			git_odb_free(odb);
		}
		if (error)
			return error;

		fc->file.size = len;
	}

	if (diff_file_content_binary_by_size(fc))
		return 0;

	if (odb_obj != NULL) {
		error = git_object__from_odb_object(
			(git_object **)&fc->blob, fc->repo, odb_obj, GIT_OBJ_BLOB);
		git_odb_object_free(odb_obj);
	} else {
		error = git_blob_lookup(
			(git_blob **)&fc->blob, fc->repo, &fc->file.oid);
	}

	if (!error) {
		fc->file.flags |= GIT_DIFF_FLAG__FREE_BLOB;
		fc->map.data = (void *)git_blob_rawcontent(fc->blob);
		fc->map.len  = (size_t)git_blob_rawsize(fc->blob);
	}

	return error;
}

static int diff_file_content_load_workdir_symlink(
	git_diff_file_content *fc, git_buf *path)
{
	ssize_t alloc_len, read_len;

	/* link path on disk could be UTF-16, so prepare a buffer that is
	 * big enough to handle some UTF-8 data expansion
	 */
	alloc_len = (ssize_t)(fc->file.size * 2) + 1;

	fc->map.data = git__calloc(alloc_len, sizeof(char));
	GITERR_CHECK_ALLOC(fc->map.data);

	fc->file.flags |= GIT_DIFF_FLAG__FREE_DATA;

	read_len = p_readlink(git_buf_cstr(path), fc->map.data, alloc_len);
	if (read_len < 0) {
		giterr_set(GITERR_OS, "Failed to read symlink '%s'", fc->file.path);
		return -1;
	}

	fc->map.len = read_len;
	return 0;
}

static int diff_file_content_load_workdir_file(
	git_diff_file_content *fc, git_buf *path)
{
	int error = 0;
	git_vector filters = GIT_VECTOR_INIT;
	git_buf raw = GIT_BUF_INIT, filtered = GIT_BUF_INIT;
	git_file fd = git_futils_open_ro(git_buf_cstr(path));

	if (fd < 0)
		return fd;

	if (!fc->file.size &&
		!(fc->file.size = git_futils_filesize(fd)))
		goto cleanup;

	if (diff_file_content_binary_by_size(fc))
		goto cleanup;

	if ((error = git_filters_load(
			&filters, fc->repo, fc->file.path, GIT_FILTER_TO_ODB)) < 0)
		goto cleanup;
	/* error >= is a filter count */

	if (error == 0) {
		if (!(error = git_futils_mmap_ro(
				&fc->map, fd, 0, (size_t)fc->file.size)))
			fc->file.flags |= GIT_DIFF_FLAG__UNMAP_DATA;
		else /* fall through to try readbuffer below */
			giterr_clear();
	}

	if (error != 0) {
		error = git_futils_readbuffer_fd(&raw, fd, (size_t)fc->file.size);
		if (error < 0)
			goto cleanup;

		if (!filters.length)
			git_buf_swap(&filtered, &raw);
		else
			error = git_filters_apply(&filtered, &raw, &filters);

		if (!error) {
			fc->map.len  = git_buf_len(&filtered);
			fc->map.data = git_buf_detach(&filtered);
			fc->file.flags |= GIT_DIFF_FLAG__FREE_DATA;
		}

		git_buf_free(&raw);
		git_buf_free(&filtered);
	}

cleanup:
	git_filters_free(&filters);
	p_close(fd);

	return error;
}

static int diff_file_content_load_workdir(git_diff_file_content *fc)
{
	int error = 0;
	git_buf path = GIT_BUF_INIT;

	if (fc->file.mode == GIT_FILEMODE_COMMIT)
		return diff_file_content_commit_to_str(fc, true);

	if (fc->file.mode == GIT_FILEMODE_TREE)
		return 0;

	if (git_buf_joinpath(
			&path, git_repository_workdir(fc->repo), fc->file.path) < 0)
		return -1;

	if (S_ISLNK(fc->file.mode))
		error = diff_file_content_load_workdir_symlink(fc, &path);
	else
		error = diff_file_content_load_workdir_file(fc, &path);

	/* once data is loaded, update OID if we didn't have it previously */
	if (!error && (fc->file.flags & GIT_DIFF_FLAG_VALID_OID) == 0) {
		error = git_odb_hash(
			&fc->file.oid, fc->map.data, fc->map.len, GIT_OBJ_BLOB);
		fc->file.flags |= GIT_DIFF_FLAG_VALID_OID;
	}

	git_buf_free(&path);
	return error;
}

int git_diff_file_content__load(git_diff_file_content *fc)
{
	int error = 0;

	if ((fc->file.flags & GIT_DIFF_FLAG__LOADED) != 0)
		return 0;

	if (fc->file.flags & GIT_DIFF_FLAG_BINARY)
		return 0;

	if (fc->src == GIT_ITERATOR_TYPE_WORKDIR)
		error = diff_file_content_load_workdir(fc);
	else
		error = diff_file_content_load_blob(fc);
	if (error)
		return error;

	fc->file.flags |= GIT_DIFF_FLAG__LOADED;

	diff_file_content_binary_by_content(fc);

	return 0;
}

void git_diff_file_content__unload(git_diff_file_content *fc)
{
	if (fc->file.flags & GIT_DIFF_FLAG__FREE_DATA) {
		git__free(fc->map.data);
		fc->map.data = "";
		fc->map.len  = 0;
		fc->file.flags &= ~GIT_DIFF_FLAG__FREE_DATA;
	}
	else if (fc->file.flags & GIT_DIFF_FLAG__UNMAP_DATA) {
		git_futils_mmap_free(&fc->map);
		fc->map.data = "";
		fc->map.len  = 0;
		fc->file.flags &= ~GIT_DIFF_FLAG__UNMAP_DATA;
	}

	if (fc->file.flags & GIT_DIFF_FLAG__FREE_BLOB) {
		git_blob_free((git_blob *)fc->blob);
		fc->blob = NULL;
		fc->file.flags &= ~GIT_DIFF_FLAG__FREE_BLOB;
	}

	fc->file.flags &= ~GIT_DIFF_FLAG__LOADED;
}

void git_diff_file_content__clear(git_diff_file_content *fc)
{
	git_diff_file_content__unload(fc);

	/* for now, nothing else to do */
}
