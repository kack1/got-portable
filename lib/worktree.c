/*
 * Copyright (c) 2018 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>
#include <sys/limits.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "got_error.h"
#include "got_repository.h"
#include "got_refs.h"
#include "got_worktree.h"

#include "got_worktree_priv.h"
#include "got_path_priv.h"

static const struct got_error *
create_meta_file(const char *gotpath, const char *name, const char *content)
{
	const struct got_error *err = NULL;
	char *path;
	int fd = -1;
	char buf[4];
	ssize_t n;

	if (asprintf(&path, "%s/%s", gotpath, name) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		path = NULL;
		goto done;
	}

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW,
	    GOT_DEFAULT_FILE_MODE);
	if (fd == -1) {
		err = got_error_from_errno();
		goto done;
	}

	/* The file should be empty. */
	n = read(fd, buf, sizeof(buf));
	if (n != 0) {
		err = (n == -1 ? got_error_from_errno() :
		    got_error(GOT_ERR_WORKTREE_EXISTS));
		goto done;
	}

	if (content) {
		int len = dprintf(fd, "%s\n", content);
		if (len != strlen(content) + 1) {
			err = got_error_from_errno();
			goto done;
		}
	}

done:
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno();
	free(path);
	return err;
}

static const struct got_error *
read_meta_file(char **content, const char *gotpath, const char *name)
{
	const struct got_error *err = NULL;
	char *path;
	int fd = -1;
	ssize_t n;
	struct stat sb;

	*content = NULL;

	if (asprintf(&path, "%s/%s", gotpath, name) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		path = NULL;
		goto done;
	}

	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd == -1) {
		err = got_error_from_errno();
		goto done;
	}
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		err = (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno());
		goto done;
	}

	stat(path, &sb);
	*content = calloc(1, sb.st_size);
	if (*content == NULL) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}

	n = read(fd, *content, sb.st_size);
	if (n != sb.st_size) {
		err = got_error_from_errno();
		goto done;
	}
	if ((*content)[sb.st_size - 1] != '\n') {
		err = got_error(GOT_ERR_WORKTREE_META);
		goto done;
	}
	(*content)[sb.st_size - 1] = '\0';

done:
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno();
	free(path);
	if (err) {
		free(*content);
		*content = NULL;
	}
	return err;
}

const struct got_error *
got_worktree_init(const char *path, struct got_reference *head_ref,
    const char *prefix, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *gotpath = NULL;
	char *refstr = NULL;
	char *path_repos = NULL;
	char *formatstr = NULL;

	if (!got_path_is_absolute(prefix))
		return got_error(GOT_ERR_BAD_PATH);

	/* Create top-level directory (may already exist). */
	if (mkdir(path, GOT_DEFAULT_DIR_MODE) == -1 && errno != EEXIST) {
		err = got_error_from_errno();
		goto done;
	}

	/* Create .got directory (may already exist). */
	if (asprintf(&gotpath, "%s/%s", path, GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	if (mkdir(gotpath, GOT_DEFAULT_DIR_MODE) == -1 && errno != EEXIST) {
		err = got_error_from_errno();
		goto done;
	}

	/* Create an empty lock file. */
	err = create_meta_file(gotpath, GOT_WORKTREE_LOCK, NULL);
	if (err)
		goto done;

	/* Create an empty file index. */
	err = create_meta_file(gotpath, GOT_WORKTREE_FILE_INDEX, NULL);
	if (err)
		goto done;

	/* Set base commit to empty. */
	err = create_meta_file(gotpath, GOT_WORKTREE_BASE_COMMIT, NULL);
	if (err)
		goto done;

	/* Write the HEAD reference. */
	refstr = got_ref_to_str(head_ref);
	if (refstr == NULL) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	err = create_meta_file(gotpath, GOT_WORKTREE_HEAD, refstr);
	if (err)
		goto done;

	/* Store path to repository. */
	path_repos = got_repo_get_path(repo);
	if (path_repos == NULL) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	err = create_meta_file(gotpath, GOT_WORKTREE_REPOSITORY, path_repos);
	if (err)
		goto done;

	/* Store in-repository path prefix. */
	err = create_meta_file(gotpath, GOT_WORKTREE_PATH_PREFIX, prefix);
	if (err)
		goto done;

	/* Stamp work tree with format file. */
	if (asprintf(&formatstr, "%d", GOT_WORKTREE_FORMAT_VERSION) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	err = create_meta_file(gotpath, GOT_WORKTREE_FORMAT, formatstr);
	if (err)
		goto done;

done:
	free(gotpath);
	free(formatstr);
	free(refstr);
	free(path_repos);
	return err;
}

const struct got_error *
got_worktree_open(struct got_worktree **worktree, const char *path)
{
	const struct got_error *err = NULL;
	char *gotpath;
	char *refstr = NULL;
	char *path_repos = NULL;
	char *formatstr = NULL;
	char *path_lock = NULL;
	int version, fd = -1;
	const char *errstr;

	*worktree = NULL;

	if (asprintf(&gotpath, "%s/%s", path, GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		gotpath = NULL;
		goto done;
	}

	if (asprintf(&path_lock, "%s/%s", gotpath, GOT_WORKTREE_LOCK) == -1) {
		err = got_error(GOT_ERR_NO_MEM);
		path_lock = NULL;
		goto done;
	}

	fd = open(path_lock, O_RDWR | O_EXLOCK | O_NONBLOCK);
	if (fd == -1) {
		err = (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno());
		goto done;
	}

	err = read_meta_file(&formatstr, gotpath, GOT_WORKTREE_FORMAT);
	if (err)
		goto done;

	version = strtonum(formatstr, 1, INT_MAX, &errstr);
	if (errstr) {
		err = got_error(GOT_ERR_WORKTREE_META);
		goto done;
	}
	if (version != GOT_WORKTREE_FORMAT_VERSION) {
		err = got_error(GOT_ERR_WORKTREE_VERS);
		goto done;
	}

	*worktree = calloc(1, sizeof(**worktree));
	if (*worktree == NULL) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	(*worktree)->lockfd = -1;

	(*worktree)->path_worktree_root = strdup(path);
	if ((*worktree)->path_worktree_root == NULL) {
		err = got_error(GOT_ERR_NO_MEM);
		goto done;
	}
	err = read_meta_file(&(*worktree)->path_repo, gotpath,
	    GOT_WORKTREE_REPOSITORY);
	if (err)
		goto done;
	err = read_meta_file(&(*worktree)->path_prefix, gotpath,
	    GOT_WORKTREE_PATH_PREFIX);
		goto done;

done:
	free(gotpath);
	free(path_lock);
	if (err) {
		if (fd != -1)
			close(fd);
		if (*worktree != NULL)
			got_worktree_close(*worktree);
		*worktree = NULL;
	} else
		(*worktree)->lockfd = fd;

	return err;
}

void
got_worktree_close(struct got_worktree *worktree)
{
	free(worktree->path_worktree_root);
	free(worktree->path_repo);
	free(worktree->path_prefix);
	if (worktree->lockfd != -1)
		close(worktree->lockfd);
	free(worktree);
}

char *
got_worktree_get_repo_path(struct got_worktree *worktree)
{
	return strdup(worktree->path_repo);
}

struct got_reference *
got_worktree_get_head(struct got_worktree *worktree)
{
	return NULL;
}

const struct got_error *
got_worktree_change_head(struct got_worktree *worktree, struct got_reference *head,
    struct got_repository *repo)
{
	return NULL;
}

const struct got_error *
got_worktree_checkout_files(struct got_worktree *worktree,
    struct got_repository *repo)
{
	return NULL;
}
