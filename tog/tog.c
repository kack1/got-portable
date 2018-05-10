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

#include <sys/queue.h>

#include <errno.h>
#include <curses.h>
#include <panel.h>
#include <locale.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <err.h>
#include <unistd.h>

#include "got_error.h"
#include "got_object.h"
#include "got_reference.h"
#include "got_repository.h"
#include "got_diff.h"

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

enum tog_view_id {
	TOG_VIEW_LOG,
	TOG_VIEW_DIFF,
	TOG_VIEW_BLAME,
};

struct tog_cmd {
	const char *name;
	const struct got_error *(*cmd_main)(int, char *[]);
	void (*cmd_usage)(void);
	enum tog_view_id view;
	const char *descr;
};

__dead void	usage(void);
__dead void	usage_log(void);
__dead void	usage_diff(void);
__dead void	usage_blame(void);

const struct got_error*	cmd_log(int, char *[]);
const struct got_error*	cmd_diff(int, char *[]);
const struct got_error*	cmd_blame(int, char *[]);

struct tog_cmd tog_commands[] = {
	{ "log",	cmd_log,	usage_log,	TOG_VIEW_LOG,
	    "show repository history" },
	{ "diff",	cmd_diff,	usage_diff,	TOG_VIEW_DIFF,
	    "compare files and directories" },
	{ "blame",	cmd_blame,	usage_blame,	TOG_VIEW_BLAME,
	    "show line-by-line file history" },
};

/* globals */
WINDOW *tog_main_win;
PANEL *tog_main_panel;
static struct tog_log_view {
	WINDOW *window;
	PANEL *panel;
} tog_log_view;

__dead void
usage_log(void)
{
	endwin();
	fprintf(stderr, "usage: %s log [-c commit] [repository-path]\n",
	    getprogname());
	exit(1);
}

static const struct got_error *
draw_commit(struct got_commit_object *commit, struct got_object_id *id)
{
	const struct got_error *err = NULL;
	char *logmsg0 = NULL, *logmsg = NULL;
	char *author0 = NULL, *author = NULL;
	char *newline, *smallerthan;
	char *line = NULL;
	char *id_str = NULL;
	const size_t id_display_len = 8;
	const size_t author_display_len = 16;
	size_t id_len, author_len, logmsg_len, avail;
	int i, col;

	err = got_object_id_str(&id_str, id);
	if (err)
		return err;
	id_len = strlen(id_str);

	logmsg0 = strdup(commit->logmsg);
	if (logmsg0 == NULL) {
		err = got_error_from_errno();
		goto done;
	}
	logmsg = logmsg0;
	while (*logmsg == '\n')
		logmsg++;
	newline = strchr(logmsg, '\n');
	if (newline)
		*newline = '\0';
	logmsg_len = strlen(logmsg);

	author0 = strdup(commit->author);
	if (author0 == NULL) {
		err = got_error_from_errno();
		goto done;
	}
	author = author0;
	smallerthan = strchr(author, '<');
	if (smallerthan)
		*smallerthan = '\0';
	else {
		char *at = strchr(author, '@');
		if (at)
			*at = '\0';
	}
	author_len = strlen(author);

	avail = COLS - 1;
	line = calloc(avail + 1, sizeof(*line));
	if (line == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	col = 0;
	for (i = 0; i < MIN(id_display_len, id_len); i++) {
		if (col >= avail)
			goto draw;
		line[col++] = id_str[i];
	}
	while (i < id_display_len) {
		if (col >= avail)
			goto draw;
		line[col++] = ' ';
		i++;
	}
	if (col >= avail)
		goto draw;
	line[col++] = ' ';
	for (i = 0; i < MIN(author_display_len, author_len); i++) {
		if (col >= avail)
			goto draw;
		line[col++] = author[i];
	}
	while (i < author_display_len) {
		if (col >= avail)
			goto draw;
		line[col++] = ' ';
		i++;
	}
	if (col >= avail)
		goto draw;
	line[col++] = ' ';

	while (col < avail && *logmsg)
		line[col++] = *logmsg++;
	while (col < avail)
		line[col++] = ' ';
draw:
	waddstr(tog_log_view.window, line);
	waddch(tog_log_view.window, '\n');
done:
	free(logmsg0);
	free(author0);
	free(line);
	free(id_str);
	return err;
}
struct commit_queue_entry {
	TAILQ_ENTRY(commit_queue_entry) entry;
	struct got_object_id *id;
	struct got_commit_object *commit;
};
TAILQ_HEAD(commit_queue, commit_queue_entry);

static struct commit_queue_entry *
alloc_commit_queue_entry(struct got_commit_object *commit,
    struct got_object_id *id)
{
	struct commit_queue_entry *entry;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;

	entry->id = id;
	entry->commit = commit;
	return entry;
}

static void
pop_commit(struct commit_queue *commits)
{
	struct commit_queue_entry *entry;

	entry = TAILQ_FIRST(commits);
	TAILQ_REMOVE(commits, entry, entry);
	got_object_commit_close(entry->commit);
	free(entry->id);
	free(entry);
}

static void
free_commits(struct commit_queue *commits)
{
	while (!TAILQ_EMPTY(commits))
		pop_commit(commits);
}


static const struct got_error *
fetch_parent_commit(struct commit_queue_entry **pentry,
    struct commit_queue_entry *entry, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object *obj = NULL;
	struct got_commit_object *commit;
	struct got_object_id *id;
	struct got_parent_id *pid;

	*pentry = NULL;

	/* Follow the first parent (TODO: handle merge commits). */
	pid = SIMPLEQ_FIRST(&entry->commit->parent_ids);
	if (pid == NULL)
		return NULL;
	err = got_object_open(&obj, repo, pid->id);
	if (err)
		return err;
	if (got_object_get_type(obj) != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		got_object_close(obj);
		return err;
	}

	err = got_object_commit_open(&commit, repo, obj);
	got_object_close(obj);
	if (err)
		return err;

	id = got_object_id_dup(pid->id);
	if (id == NULL) {
		err = got_error_from_errno();
		got_object_commit_close(commit);
		return err;;
	}

	*pentry = alloc_commit_queue_entry(commit, id);
	if (*pentry == NULL) {
		err = got_error_from_errno();
		got_object_commit_close(commit);
	}

	return err;;
}

static const struct got_error *
get_head_commit_id(struct got_object_id **head_id, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_reference *head_ref;

	*head_id = NULL;

	err = got_ref_open(&head_ref, repo, GOT_REF_HEAD);
	if (err)
		return err;

	err = got_ref_resolve(head_id, repo, head_ref);
	got_ref_close(head_ref);
	if (err) {
		*head_id = NULL;
		return err;
	}

	return NULL;
}

static const struct got_error *
prepend_commits(int *ncommits, struct commit_queue *commits,
    struct got_object_id *first_id, struct got_object_id *last_id,
    int limit, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object *first_obj = NULL, *last_obj = NULL;
	struct got_commit_object *commit = NULL;
	struct got_object_id *id = NULL;
	struct commit_queue_entry *entry, *old_head_entry;

	*ncommits = 0;

	err = got_object_open(&first_obj, repo, first_id);
	if (err)
		goto done;
	if (got_object_get_type(first_obj) != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}
	err = got_object_open(&last_obj, repo, last_id);
	if (err)
		goto done;
	if (got_object_get_type(last_obj) != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_commit_open(&commit, repo, first_obj);
	if (err)
		goto done;

	id = got_object_id_dup(first_id);
	if (id == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	entry = alloc_commit_queue_entry(commit, id);
	if (entry == NULL)
		return got_error_from_errno();

	old_head_entry = TAILQ_FIRST(commits);
	if (old_head_entry)
		TAILQ_INSERT_BEFORE(old_head_entry, entry, entry);
	else
		TAILQ_INSERT_HEAD(commits, entry, entry);

	*ncommits = 1;

	/*
	 * Fetch parent commits.
	 * XXX If first and last commit aren't ancestrally related this loop
	 * we will keep iterating until a root commit is encountered.
	 */
	while (1) {
		struct commit_queue_entry *pentry;

		err = fetch_parent_commit(&pentry, entry, repo);
		if (err)
			goto done;
		if (pentry == NULL)
			break;

		/*
		 * Fill up to old HEAD commit if commit queue was not empty.
		 * We must not leave a gap in history.
		 */
		if (old_head_entry &&
		    got_object_id_cmp(pentry->id, old_head_entry->id) == 0)
			break;

		TAILQ_INSERT_AFTER(commits, entry, pentry, entry);
		(*ncommits)++;
		if (*ncommits >= limit)
			break;

		/* Fill up to last requested commit if queue was empty. */
		if (old_head_entry == NULL &&
		    got_object_id_cmp(pentry->id, last_id) == 0)
			break;

		entry = pentry;
	}

done:
	if (first_obj)
		got_object_close(first_obj);
	if (last_obj)
		got_object_close(last_obj);
	return err;
}

static const struct got_error *
fetch_commits(struct commit_queue_entry **start_entry,
    struct got_object_id *start_id, struct commit_queue *commits,
    int limit, struct got_repository *repo)
{
	const struct got_error *err;
	struct commit_queue_entry *entry;
	int ncommits = 0;
	struct got_object_id *head_id = NULL;

	*start_entry = NULL;

	err = get_head_commit_id(&head_id, repo);
	if (err)
		return err;

	/* Prepend HEAD commit and all ancestors up to start commit. */
	err = prepend_commits(&ncommits, commits, head_id, start_id, limit,
	    repo);
	if (err)
		return err;

	if (got_object_id_cmp(head_id, start_id) == 0)
		*start_entry = TAILQ_FIRST(commits);
	else
		*start_entry = TAILQ_LAST(commits, commit_queue);

	if (ncommits >= limit)
		return NULL;

	/* Append more commits from start commit up to the requested limit. */
	entry = TAILQ_LAST(commits, commit_queue);
	while (entry && ncommits < limit) {
		struct commit_queue_entry *pentry;

		err = fetch_parent_commit(&pentry, entry, repo);
		if (err)
			break;
		if (pentry)
			TAILQ_INSERT_TAIL(commits, pentry, entry);
		entry = pentry;
		ncommits++;
	}

	if (err)
		*start_entry = NULL;
	return err;
}

static const struct got_error *
draw_commits(struct commit_queue_entry **last,
    struct commit_queue_entry *first, int selected, int limit)
{
	const struct got_error *err = NULL;
	struct commit_queue_entry *entry;
	int ncommits = 0;

	wclear(tog_log_view.window);

	entry = first;
	*last = first;
	while (entry) {
		if (ncommits == limit)
			break;
		if (ncommits == selected)
			wstandout(tog_log_view.window);
		err = draw_commit(entry->commit, entry->id);
		if (ncommits == selected)
			wstandend(tog_log_view.window);
		if (err)
			break;
		ncommits++;
		*last = entry;
		entry = TAILQ_NEXT(entry, entry);
	}

	update_panels();
	doupdate();

	return err;
}

static const struct got_error *
show_log_view(struct got_object_id *start_id, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *id;
	int ch, done = 0, selected = 0;
	struct commit_queue commits;
	struct commit_queue_entry *pentry = NULL;
	struct commit_queue_entry *first_displayed_entry = NULL;
	struct commit_queue_entry *last_displayed_entry = NULL;

	id = got_object_id_dup(start_id);
	if (id == NULL)
		return got_error_from_errno();

	if (tog_log_view.window == NULL) {
		tog_log_view.window = newwin(0, 0, 0, 0);
		if (tog_log_view.window == NULL)
			return got_error_from_errno();
		keypad(tog_log_view.window, TRUE);
	}
	if (tog_log_view.panel == NULL) {
		tog_log_view.panel = new_panel(tog_log_view.window);
		if (tog_log_view.panel == NULL)
			return got_error_from_errno();
	}

	TAILQ_INIT(&commits);
	err = fetch_commits(&first_displayed_entry, id, &commits, LINES, repo);
	if (err)
		goto done;
	while (!done) {
		err = draw_commits(&last_displayed_entry, first_displayed_entry,
		    selected, LINES);
		if (err)
			goto done;

		nodelay(stdscr, FALSE);
		ch = wgetch(tog_log_view.window);
		switch (ch) {
			case ERR:
				if (errno) {
					err = got_error_from_errno();
					goto done;
				}
				break;
			case 'q':
				done = 1;
				break;
			case 'k':
			case KEY_UP:
				if (selected > 0)
					selected--;
				break;
			case 'j':
			case KEY_DOWN:
				if (selected < LINES - 1)
					selected++;
				if (selected < LINES - 1)
					break;

				/* scroll down if there are more parents */
				if (last_displayed_entry->commit->nparents == 0)
					break;
				first_displayed_entry =
				    TAILQ_NEXT(first_displayed_entry, entry);
				err = fetch_parent_commit(&pentry,
				    last_displayed_entry, repo);
				if (err)
					break;
				if (pentry)
					TAILQ_INSERT_TAIL(&commits, pentry,
					    entry);
				break;
			case KEY_RESIZE:
				if (selected > LINES)
					selected = LINES - 1;
				break;
			default:
				break;
		}
		nodelay(stdscr, TRUE);
	}
done:
	free_commits(&commits);
	return err;
}

const struct got_error *
cmd_log(int argc, char *argv[])
{
	const struct got_error *error;
	struct got_repository *repo;
	struct got_object_id *start_id = NULL;
	char *repo_path = NULL;
	char *start_commit = NULL;
	int ch;

#ifndef PROFILE
	if (pledge("stdio rpath wpath cpath flock proc tty", NULL) == -1)
		err(1, "pledge");
#endif

	while ((ch = getopt(argc, argv, "c:")) != -1) {
		switch (ch) {
		case 'c':
			start_commit = optarg;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc == 0) {
		repo_path = getcwd(NULL, 0);
		if (repo_path == NULL)
			return got_error_from_errno();
	} else if (argc == 1) {
		repo_path = realpath(argv[0], NULL);
		if (repo_path == NULL)
			return got_error_from_errno();
	} else
		usage_log();

	error = got_repo_open(&repo, repo_path);
	free(repo_path);
	if (error != NULL)
		return error;

	if (start_commit == NULL) {
		error = get_head_commit_id(&start_id, repo);
		if (error != NULL)
			return error;
	} else {
		struct got_object *obj;
		error = got_object_open_by_id_str(&obj, repo, start_commit);
		if (error == NULL) {
			start_id = got_object_get_id(obj);
			if (start_id == NULL)
				error = got_error_from_errno();
		}
	}
	if (error != NULL)
		return error;
	error = show_log_view(start_id, repo);
	free(start_id);
	got_repo_close(repo);
	return error;
}

__dead void
usage_diff(void)
{
	endwin();
	fprintf(stderr, "usage: %s diff [repository-path] object1 object2\n",
	    getprogname());
	exit(1);
}

const struct got_error *
cmd_diff(int argc, char *argv[])
{
	return got_error(GOT_ERR_NOT_IMPL);
}

__dead void
usage_blame(void)
{
	endwin();
	fprintf(stderr, "usage: %s blame [repository-path] blob-object\n",
	    getprogname());
	exit(1);
}

const struct got_error *
cmd_blame(int argc, char *argv[])
{
	return got_error(GOT_ERR_NOT_IMPL);
}

static const struct got_error *
init_curses(void)
{
	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);

	tog_main_win = newwin(0, 0, 0, 0);
	if (tog_main_win == NULL)
		return got_error_from_errno();
	tog_main_panel = new_panel(tog_main_win);
	if (tog_main_panel == NULL)
		return got_error_from_errno();

	return NULL;
}

__dead void
usage(void)
{
	int i;

	fprintf(stderr, "usage: %s [-h] [command] [arg ...]\n\n"
	    "Available commands:\n", getprogname());
	for (i = 0; i < nitems(tog_commands); i++) {
		struct tog_cmd *cmd = &tog_commands[i];
		fprintf(stderr, "    %s: %s\n", cmd->name, cmd->descr);
	}
	exit(1);
}

static char **
make_argv(const char *arg0, const char *arg1)
{
	char **argv;
	int argc = (arg1 == NULL ? 1 : 2);

	argv = calloc(argc, sizeof(char *));
	if (argv == NULL)
		err(1, "calloc");
	argv[0] = strdup(arg0);
	if (argv[0] == NULL)
		err(1, "calloc");
	if (arg1) {
		argv[1] = strdup(arg1);
		if (argv[1] == NULL)
			err(1, "calloc");
	}

	return argv;
}

int
main(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	struct tog_cmd *cmd = NULL;
	int ch, hflag = 0;
	char **cmd_argv = NULL;

	setlocale(LC_ALL, "");

	while ((ch = getopt(argc, argv, "h")) != -1) {
		switch (ch) {
		case 'h':
			hflag = 1;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;
	optreset = 1;

	if (argc == 0) {
		/* Build an argument vector which runs a default command. */
		cmd = &tog_commands[0];
		cmd_argv = make_argv(cmd->name, NULL);
		argc = 1;
	} else {
		int i;

		/* Did the user specific a command? */
		for (i = 0; i < nitems(tog_commands); i++) {
			if (strncmp(tog_commands[i].name, argv[0],
			    strlen(argv[0])) == 0) {
				cmd = &tog_commands[i];
				if (hflag)
					tog_commands[i].cmd_usage();
				break;
			}
		}
		if (cmd == NULL) {
			/* Did the user specify a repository? */
			char *repo_path = realpath(argv[0], NULL);
			if (repo_path) {
				struct got_repository *repo;
				error = got_repo_open(&repo, repo_path);
				if (error == NULL)
					got_repo_close(repo);
			} else
				error = got_error_from_errno();
			if (error) {
				fprintf(stderr, "%s: '%s' is neither a known "
				    "command nor a path to a repository\n",
				    getprogname(), argv[0]);
				free(repo_path);
				return 1;
			}
			cmd = &tog_commands[0];
			cmd_argv = make_argv(cmd->name, repo_path);
			argc = 2;
			free(repo_path);
		}
	}

	error = init_curses();
	if (error) {
		fprintf(stderr, "cannot initialize ncurses: %s\n", error->msg);
		return 1;
	}

	error = cmd->cmd_main(argc, cmd_argv ? cmd_argv : argv);
	if (error)
		goto done;
done:
	endwin();
	free(cmd_argv);
	if (error)
		fprintf(stderr, "%s: %s\n", getprogname(), error->msg);
	return 0;
}
