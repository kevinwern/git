/*
 * Builtin "git clone"
 *
 * Copyright (c) 2007 Kristian Høgsberg <krh@redhat.com>,
 *		 2008 Daniel Barkalow <barkalow@iabervon.org>
 * Based on git-commit.sh by Junio C Hamano and Linus Torvalds
 *
 * Clone a repository into a different directory that does not yet exist.
 */

#include "cache.h"
#include "builtin.h"
#include "bundle.h"
#include "lockfile.h"
#include "parse-options.h"
#include "fetch-pack.h"
#include "refs.h"
#include "tree.h"
#include "tree-walk.h"
#include "unpack-trees.h"
#include "transport.h"
#include "strbuf.h"
#include "dir.h"
#include "sigchain.h"
#include "branch.h"
#include "remote.h"
#include "run-command.h"
#include "connected.h"

/*
 * Overall FIXMEs:
 *  - respect DB_ENVIRONMENT for .git/objects.
 *
 * Implementation notes:
 *  - dropping use-separate-remote and no-separate-remote compatibility
 *
 */
static const char * const builtin_clone_usage[] = {
	N_("git clone [<options>] [--] <repo> [<dir>]"),
	NULL
};

static int option_no_checkout, option_bare, option_mirror, option_single_branch = -1;
static int option_local = -1, option_no_hardlinks, option_shared, option_recursive;
static int option_resume;
static char *option_template, *option_depth;
static const char *option_origin = NULL;
static char *option_branch = NULL;
static const char *real_git_dir;
static char *option_upload_pack = "git-upload-pack";
static char *option_prime_clone = "git-prime-clone";
static int option_verbosity;
static int option_progress = -1;
static enum transport_family family;
static struct string_list option_config;
static struct string_list option_reference;
static int option_dissociate;
static const struct alt_resource *alt_res = NULL;

static struct option builtin_clone_options[] = {
	OPT__VERBOSITY(&option_verbosity),
	OPT_BOOL(0, "progress", &option_progress,
		 N_("force progress reporting")),
	OPT_BOOL('n', "no-checkout", &option_no_checkout,
		 N_("don't create a checkout")),
	OPT_BOOL(0, "bare", &option_bare, N_("create a bare repository")),
	OPT_HIDDEN_BOOL(0, "naked", &option_bare,
			N_("create a bare repository")),
	OPT_BOOL(0, "mirror", &option_mirror,
		 N_("create a mirror repository (implies bare)")),
	OPT_BOOL('l', "local", &option_local,
		N_("to clone from a local repository")),
	OPT_BOOL(0, "no-hardlinks", &option_no_hardlinks,
		    N_("don't use local hardlinks, always copy")),
	OPT_BOOL('s', "shared", &option_shared,
		    N_("setup as shared repository")),
	OPT_BOOL(0, "recursive", &option_recursive,
		    N_("initialize submodules in the clone")),
	OPT_BOOL(0, "recurse-submodules", &option_recursive,
		    N_("initialize submodules in the clone")),
	OPT_STRING(0, "template", &option_template, N_("template-directory"),
		   N_("directory from which templates will be used")),
	OPT_STRING_LIST(0, "reference", &option_reference, N_("repo"),
			N_("reference repository")),
	OPT_BOOL(0, "dissociate", &option_dissociate,
		 N_("use --reference only while cloning")),
	OPT_STRING('o', "origin", &option_origin, N_("name"),
		   N_("use <name> instead of 'origin' to track upstream")),
	OPT_STRING('b', "branch", &option_branch, N_("branch"),
		   N_("checkout <branch> instead of the remote's HEAD")),
	OPT_STRING('u', "upload-pack", &option_upload_pack, N_("path"),
		   N_("path to git-upload-pack on the remote")),
	OPT_STRING('p', "prime-clone", &option_prime_clone, N_("path"),
		   N_("path to git-prime-clone on the remote")),
	OPT_STRING(0, "depth", &option_depth, N_("depth"),
		    N_("create a shallow clone of that depth")),
	OPT_BOOL(0, "single-branch", &option_single_branch,
		    N_("clone only one branch, HEAD or --branch")),
	OPT_BOOL(0, "resume", &option_resume,
		    N_("continue a resumable clone")),
	OPT_STRING(0, "separate-git-dir", &real_git_dir, N_("gitdir"),
		   N_("separate git dir from working tree")),
	OPT_STRING_LIST('c', "config", &option_config, N_("key=value"),
			N_("set config inside the new repository")),
	OPT_SET_INT('4', "ipv4", &family, N_("use IPv4 addresses only"),
			TRANSPORT_FAMILY_IPV4),
	OPT_SET_INT('6', "ipv6", &family, N_("use IPv6 addresses only"),
			TRANSPORT_FAMILY_IPV6),
	OPT_END()
};

static const char *argv_submodule[] = {
	"submodule", "update", "--init", "--recursive", NULL
};

static const char *get_repo_path_1(struct strbuf *path, int *is_bundle)
{
	static char *suffix[] = { "/.git", "", ".git/.git", ".git" };
	static char *bundle_suffix[] = { ".bundle", "" };
	size_t baselen = path->len;
	struct stat st;
	int i;

	for (i = 0; i < ARRAY_SIZE(suffix); i++) {
		strbuf_setlen(path, baselen);
		strbuf_addstr(path, suffix[i]);
		if (stat(path->buf, &st))
			continue;
		if (S_ISDIR(st.st_mode) && is_git_directory(path->buf)) {
			*is_bundle = 0;
			return path->buf;
		} else if (S_ISREG(st.st_mode) && st.st_size > 8) {
			/* Is it a "gitfile"? */
			char signature[8];
			const char *dst;
			int len, fd = open(path->buf, O_RDONLY);
			if (fd < 0)
				continue;
			len = read_in_full(fd, signature, 8);
			close(fd);
			if (len != 8 || strncmp(signature, "gitdir: ", 8))
				continue;
			dst = read_gitfile(path->buf);
			if (dst) {
				*is_bundle = 0;
				return dst;
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(bundle_suffix); i++) {
		strbuf_setlen(path, baselen);
		strbuf_addstr(path, bundle_suffix[i]);
		if (!stat(path->buf, &st) && S_ISREG(st.st_mode)) {
			*is_bundle = 1;
			return path->buf;
		}
	}

	return NULL;
}

static char *get_repo_path(const char *repo, int *is_bundle)
{
	struct strbuf path = STRBUF_INIT;
	const char *raw;
	char *canon;

	strbuf_addstr(&path, repo);
	raw = get_repo_path_1(&path, is_bundle);
	canon = raw ? xstrdup(absolute_path(raw)) : NULL;
	strbuf_release(&path);
	return canon;
}

static char *guess_dir_name(const char *repo, int is_bundle, int is_bare)
{
	const char *end = repo + strlen(repo), *start, *ptr;
	size_t len;
	char *dir;

	/*
	 * Skip scheme.
	 */
	start = strstr(repo, "://");
	if (start == NULL)
		start = repo;
	else
		start += 3;

	/*
	 * Skip authentication data. The stripping does happen
	 * greedily, such that we strip up to the last '@' inside
	 * the host part.
	 */
	for (ptr = start; ptr < end && !is_dir_sep(*ptr); ptr++) {
		if (*ptr == '@')
			start = ptr + 1;
	}

	/*
	 * Strip trailing spaces, slashes and /.git
	 */
	while (start < end && (is_dir_sep(end[-1]) || isspace(end[-1])))
		end--;
	if (end - start > 5 && is_dir_sep(end[-5]) &&
	    !strncmp(end - 4, ".git", 4)) {
		end -= 5;
		while (start < end && is_dir_sep(end[-1]))
			end--;
	}

	/*
	 * Strip trailing port number if we've got only a
	 * hostname (that is, there is no dir separator but a
	 * colon). This check is required such that we do not
	 * strip URI's like '/foo/bar:2222.git', which should
	 * result in a dir '2222' being guessed due to backwards
	 * compatibility.
	 */
	if (memchr(start, '/', end - start) == NULL
	    && memchr(start, ':', end - start) != NULL) {
		ptr = end;
		while (start < ptr && isdigit(ptr[-1]) && ptr[-1] != ':')
			ptr--;
		if (start < ptr && ptr[-1] == ':')
			end = ptr - 1;
	}

	/*
	 * Find last component. To remain backwards compatible we
	 * also regard colons as path separators, such that
	 * cloning a repository 'foo:bar.git' would result in a
	 * directory 'bar' being guessed.
	 */
	ptr = end;
	while (start < ptr && !is_dir_sep(ptr[-1]) && ptr[-1] != ':')
		ptr--;
	start = ptr;

	/*
	 * Strip .{bundle,git}.
	 */
	len = end - start;
	strip_suffix_mem(start, &len, is_bundle ? ".bundle" : ".git");

	if (!len || (len == 1 && *start == '/'))
	    die("No directory name could be guessed.\n"
		"Please specify a directory on the command line");

	if (is_bare)
		dir = xstrfmt("%.*s.git", (int)len, start);
	else
		dir = xstrndup(start, len);
	/*
	 * Replace sequences of 'control' characters and whitespace
	 * with one ascii space, remove leading and trailing spaces.
	 */
	if (*dir) {
		char *out = dir;
		int prev_space = 1 /* strip leading whitespace */;
		for (end = dir; *end; ++end) {
			char ch = *end;
			if ((unsigned char)ch < '\x20')
				ch = '\x20';
			if (isspace(ch)) {
				if (prev_space)
					continue;
				prev_space = 1;
			} else
				prev_space = 0;
			*out++ = ch;
		}
		*out = '\0';
		if (out > dir && prev_space)
			out[-1] = '\0';
	}
	return dir;
}

static void strip_trailing_slashes(char *dir)
{
	char *end = dir + strlen(dir);

	while (dir < end - 1 && is_dir_sep(end[-1]))
		end--;
	*end = '\0';
}

static char *get_filename(const char *dir)
{
	char *dir_copy = xstrdup(dir);
	strip_trailing_slashes(dir_copy);
	char *filename, *final = NULL;

	filename = find_last_dir_sep(dir);

	if (filename && *(++filename))
		final = xstrdup(filename);

	free(dir_copy);
	return final;
}

static int add_one_reference(struct string_list_item *item, void *cb_data)
{
	char *ref_git;
	const char *repo;
	struct strbuf alternate = STRBUF_INIT;

	/* Beware: read_gitfile(), real_path() and mkpath() return static buffer */
	ref_git = xstrdup(real_path(item->string));

	repo = read_gitfile(ref_git);
	if (!repo)
		repo = read_gitfile(mkpath("%s/.git", ref_git));
	if (repo) {
		free(ref_git);
		ref_git = xstrdup(repo);
	}

	if (!repo && is_directory(mkpath("%s/.git/objects", ref_git))) {
		char *ref_git_git = mkpathdup("%s/.git", ref_git);
		free(ref_git);
		ref_git = ref_git_git;
	} else if (!is_directory(mkpath("%s/objects", ref_git))) {
		struct strbuf sb = STRBUF_INIT;
		if (get_common_dir(&sb, ref_git))
			die(_("reference repository '%s' as a linked checkout is not supported yet."),
			    item->string);
		die(_("reference repository '%s' is not a local repository."),
		    item->string);
	}

	if (!access(mkpath("%s/shallow", ref_git), F_OK))
		die(_("reference repository '%s' is shallow"), item->string);

	if (!access(mkpath("%s/info/grafts", ref_git), F_OK))
		die(_("reference repository '%s' is grafted"), item->string);

	strbuf_addf(&alternate, "%s/objects", ref_git);
	add_to_alternates_file(alternate.buf);
	strbuf_release(&alternate);
	free(ref_git);
	return 0;
}

static void setup_reference(void)
{
	for_each_string_list(&option_reference, add_one_reference, NULL);
}

static void copy_alternates(struct strbuf *src, struct strbuf *dst,
			    const char *src_repo)
{
	/*
	 * Read from the source objects/info/alternates file
	 * and copy the entries to corresponding file in the
	 * destination repository with add_to_alternates_file().
	 * Both src and dst have "$path/objects/info/alternates".
	 *
	 * Instead of copying bit-for-bit from the original,
	 * we need to append to existing one so that the already
	 * created entry via "clone -s" is not lost, and also
	 * to turn entries with paths relative to the original
	 * absolute, so that they can be used in the new repository.
	 */
	FILE *in = fopen(src->buf, "r");
	struct strbuf line = STRBUF_INIT;

	while (strbuf_getline(&line, in) != EOF) {
		char *abs_path;
		if (!line.len || line.buf[0] == '#')
			continue;
		if (is_absolute_path(line.buf)) {
			add_to_alternates_file(line.buf);
			continue;
		}
		abs_path = mkpathdup("%s/objects/%s", src_repo, line.buf);
		normalize_path_copy(abs_path, abs_path);
		add_to_alternates_file(abs_path);
		free(abs_path);
	}
	strbuf_release(&line);
	fclose(in);
}

static void copy_or_link_directory(struct strbuf *src, struct strbuf *dest,
				   const char *src_repo, int src_baselen)
{
	struct dirent *de;
	struct stat buf;
	int src_len, dest_len;
	DIR *dir;

	dir = opendir(src->buf);
	if (!dir)
		die_errno(_("failed to open '%s'"), src->buf);

	if (mkdir(dest->buf, 0777)) {
		if (errno != EEXIST)
			die_errno(_("failed to create directory '%s'"), dest->buf);
		else if (stat(dest->buf, &buf))
			die_errno(_("failed to stat '%s'"), dest->buf);
		else if (!S_ISDIR(buf.st_mode))
			die(_("%s exists and is not a directory"), dest->buf);
	}

	strbuf_addch(src, '/');
	src_len = src->len;
	strbuf_addch(dest, '/');
	dest_len = dest->len;

	while ((de = readdir(dir)) != NULL) {
		strbuf_setlen(src, src_len);
		strbuf_addstr(src, de->d_name);
		strbuf_setlen(dest, dest_len);
		strbuf_addstr(dest, de->d_name);
		if (stat(src->buf, &buf)) {
			warning (_("failed to stat %s\n"), src->buf);
			continue;
		}
		if (S_ISDIR(buf.st_mode)) {
			if (de->d_name[0] != '.')
				copy_or_link_directory(src, dest,
						       src_repo, src_baselen);
			continue;
		}

		/* Files that cannot be copied bit-for-bit... */
		if (!strcmp(src->buf + src_baselen, "/info/alternates")) {
			copy_alternates(src, dest, src_repo);
			continue;
		}

		if (unlink(dest->buf) && errno != ENOENT)
			die_errno(_("failed to unlink '%s'"), dest->buf);
		if (!option_no_hardlinks) {
			if (!link(src->buf, dest->buf))
				continue;
			if (option_local > 0)
				die_errno(_("failed to create link '%s'"), dest->buf);
			option_no_hardlinks = 1;
		}
		if (copy_file_with_time(dest->buf, src->buf, 0666))
			die_errno(_("failed to copy file to '%s'"), dest->buf);
	}
	closedir(dir);
}

static void clone_local(const char *src_repo, const char *dest_repo)
{
	if (option_shared) {
		struct strbuf alt = STRBUF_INIT;
		strbuf_addf(&alt, "%s/objects", src_repo);
		add_to_alternates_file(alt.buf);
		strbuf_release(&alt);
	} else {
		struct strbuf src = STRBUF_INIT;
		struct strbuf dest = STRBUF_INIT;
		get_common_dir(&src, src_repo);
		get_common_dir(&dest, dest_repo);
		strbuf_addstr(&src, "/objects");
		strbuf_addstr(&dest, "/objects");
		copy_or_link_directory(&src, &dest, src_repo, src.len);
		strbuf_release(&src);
		strbuf_release(&dest);
	}

	if (0 <= option_verbosity)
		fprintf(stderr, _("done.\n"));
}

static const char *junk_work_tree;
static const char *junk_git_dir;
static enum {
	JUNK_LEAVE_NONE,
	JUNK_LEAVE_RESUMABLE,
	JUNK_LEAVE_REPO,
	JUNK_LEAVE_ALL
} junk_mode = JUNK_LEAVE_NONE;

static const char junk_leave_repo_msg[] =
N_("Clone succeeded, but checkout failed.\n"
   "You can inspect what was checked out with 'git status'\n"
   "and retry the checkout with 'git checkout -f HEAD'\n");

static const char junk_leave_resumable_msg[] =
N_("Clone interrupted while copying resumable resource.\n"
   "Try using 'git clone --resume <new_directory>',\n"
   "where <new_directory> is either the new working \n"
   "directory or git directory.\n\n"
   "If this does not succeed, it could be because the\n"
   "resource has been moved, corrupted, or changed.\n"
   "If this is the case, you should remove <new_directory>\n"
   "and run the original command.\n");

static void write_resumable_resource()
{
	const char *filename = git_path_resumable();
	struct strbuf content = STRBUF_INIT;
	strbuf_addf(&content, "%s\n%s\n", alt_res->url, alt_res->filetype);
	int fd = open(filename, O_WRONLY | O_CREAT, 0666);
	if (fd < 0)
		die_errno(_("Could not open '%s' for writing"), filename);
	if (write_in_full(fd, content.buf, content.len) != content.len)
		die_errno(_("Could not write to '%s'"), filename);
	close(fd);
}

static void remove_junk(void)
{
	struct strbuf sb = STRBUF_INIT;

	switch (junk_mode) {
	case JUNK_LEAVE_REPO:
		warning("%s", _(junk_leave_repo_msg));
		return;
	case JUNK_LEAVE_RESUMABLE:
		write_resumable_resource();
		warning("%s", _(junk_leave_resumable_msg));
		return;
	case JUNK_LEAVE_ALL:
		return;
	default:
		/* proceed to removal */
		break;
	}

	if (junk_git_dir) {
		strbuf_addstr(&sb, junk_git_dir);
		remove_dir_recursively(&sb, 0);
		strbuf_reset(&sb);
	}
	if (junk_work_tree) {
		strbuf_addstr(&sb, junk_work_tree);
		remove_dir_recursively(&sb, 0);
		strbuf_reset(&sb);
	}
}

static void remove_junk_on_signal(int signo)
{
	remove_junk();
	sigchain_pop(signo);
	raise(signo);
}

static struct ref *find_remote_branch(const struct ref *refs, const char *branch)
{
	struct ref *ref;
	struct strbuf head = STRBUF_INIT;
	strbuf_addstr(&head, "refs/heads/");
	strbuf_addstr(&head, branch);
	ref = find_ref_by_name(refs, head.buf);
	strbuf_release(&head);

	if (ref)
		return ref;

	strbuf_addstr(&head, "refs/tags/");
	strbuf_addstr(&head, branch);
	ref = find_ref_by_name(refs, head.buf);
	strbuf_release(&head);

	return ref;
}

static struct ref *wanted_peer_refs(const struct ref *refs,
		struct refspec *refspec)
{
	struct ref *head = copy_ref(find_ref_by_name(refs, "HEAD"));
	struct ref *local_refs = head;
	struct ref **tail = head ? &head->next : &local_refs;

	if (option_single_branch) {
		struct ref *remote_head = NULL;

		if (!option_branch)
			remote_head = guess_remote_head(head, refs, 0);
		else {
			local_refs = NULL;
			tail = &local_refs;
			remote_head = copy_ref(find_remote_branch(refs, option_branch));
		}

		if (!remote_head && option_branch)
			warning(_("Could not find remote branch %s to clone."),
				option_branch);
		else {
			get_fetch_map(remote_head, refspec, &tail, 0);

			/* if --branch=tag, pull the requested tag explicitly */
			get_fetch_map(remote_head, tag_refspec, &tail, 0);
		}
	} else
		get_fetch_map(refs, refspec, &tail, 0);

	if (!option_mirror && !option_single_branch)
		get_fetch_map(refs, tag_refspec, &tail, 0);

	return local_refs;
}

static void write_remote_refs(const struct ref *local_refs)
{
	const struct ref *r;

	struct ref_transaction *t;
	struct strbuf err = STRBUF_INIT;

	t = ref_transaction_begin(&err);
	if (!t)
		die("%s", err.buf);

	for (r = local_refs; r; r = r->next) {
		if (!r->peer_ref || ref_exists(r->peer_ref->name))
			continue;
		if (ref_transaction_create(t, r->peer_ref->name, r->old_oid.hash,
					   0, NULL, &err))
			die("%s", err.buf);
	}

	if (initial_ref_transaction_commit(t, &err))
		die("%s", err.buf);

	strbuf_release(&err);
	ref_transaction_free(t);
}

static void write_followtags(const struct ref *refs, const char *msg)
{
	const struct ref *ref;
	for (ref = refs; ref; ref = ref->next) {
		if (!starts_with(ref->name, "refs/tags/"))
			continue;
		if (ends_with(ref->name, "^{}"))
			continue;
		if (!has_object_file(&ref->old_oid))
			continue;
		update_ref(msg, ref->name, ref->old_oid.hash,
			   NULL, 0, UPDATE_REFS_DIE_ON_ERR);
	}
}

static int iterate_ref_map(void *cb_data, unsigned char sha1[20])
{
	struct ref **rm = cb_data;
	struct ref *ref = *rm;

	/*
	 * Skip anything missing a peer_ref, which we are not
	 * actually going to write a ref for.
	 */
	while (ref && !ref->peer_ref)
		ref = ref->next;
	/* Returning -1 notes "end of list" to the caller. */
	if (!ref)
		return -1;

	hashcpy(sha1, ref->old_oid.hash);
	*rm = ref->next;
	return 0;
}

static void update_remote_refs(const struct ref *refs,
			       const struct ref *mapped_refs,
			       const struct ref *remote_head_points_at,
			       const char *branch_top,
			       const char *msg,
			       struct transport *transport,
			       int check_connectivity)
{
	const struct ref *rm = mapped_refs;

	if (check_connectivity) {
		if (transport->progress)
			fprintf(stderr, _("Checking connectivity... "));
		if (check_everything_connected_with_transport(iterate_ref_map,
							      0, &rm, transport))
			die(_("remote did not send all necessary objects"));
		if (transport->progress)
			fprintf(stderr, _("done.\n"));
	}

	if (refs) {
		write_remote_refs(mapped_refs);
		if (option_single_branch)
			write_followtags(refs, msg);
	}

	if (remote_head_points_at && !option_bare) {
		struct strbuf head_ref = STRBUF_INIT;
		strbuf_addstr(&head_ref, branch_top);
		strbuf_addstr(&head_ref, "HEAD");
		if (create_symref(head_ref.buf,
				  remote_head_points_at->peer_ref->name,
				  msg) < 0)
			die("unable to update %s", head_ref.buf);
		strbuf_release(&head_ref);
	}
}

static void update_head(const struct ref *our, const struct ref *remote,
			const char *msg)
{
	const char *head;
	if (our && skip_prefix(our->name, "refs/heads/", &head)) {
		/* Local default branch link */
		if (create_symref("HEAD", our->name, NULL) < 0)
			die("unable to update HEAD");
		if (!option_bare) {
			update_ref(msg, "HEAD", our->old_oid.hash, NULL, 0,
				   UPDATE_REFS_DIE_ON_ERR);
			install_branch_config(0, head, option_origin, our->name);
		}
	} else if (our) {
		struct commit *c = lookup_commit_reference(our->old_oid.hash);
		/* --branch specifies a non-branch (i.e. tags), detach HEAD */
		update_ref(msg, "HEAD", c->object.oid.hash,
			   NULL, REF_NODEREF, UPDATE_REFS_DIE_ON_ERR);
	} else if (remote) {
		/*
		 * We know remote HEAD points to a non-branch, or
		 * HEAD points to a branch but we don't know which one.
		 * Detach HEAD in all these cases.
		 */
		update_ref(msg, "HEAD", remote->old_oid.hash,
			   NULL, REF_NODEREF, UPDATE_REFS_DIE_ON_ERR);
	}
}

static int checkout(void)
{
	unsigned char sha1[20];
	char *head;
	struct lock_file *lock_file;
	struct unpack_trees_options opts;
	struct tree *tree;
	struct tree_desc t;
	int err = 0;

	if (option_no_checkout)
		return 0;

	head = resolve_refdup("HEAD", RESOLVE_REF_READING, sha1, NULL);
	if (!head) {
		warning(_("remote HEAD refers to nonexistent ref, "
			  "unable to checkout.\n"));
		return 0;
	}
	if (!strcmp(head, "HEAD")) {
		if (advice_detached_head)
			detach_advice(sha1_to_hex(sha1));
	} else {
		if (!starts_with(head, "refs/heads/"))
			die(_("HEAD not found below refs/heads!"));
	}
	free(head);

	/* We need to be in the new work tree for the checkout */
	setup_work_tree();

	lock_file = xcalloc(1, sizeof(struct lock_file));
	hold_locked_index(lock_file, 1);

	memset(&opts, 0, sizeof opts);
	opts.update = 1;
	opts.merge = 1;
	opts.fn = oneway_merge;
	opts.verbose_update = (option_verbosity >= 0);
	opts.src_index = &the_index;
	opts.dst_index = &the_index;

	tree = parse_tree_indirect(sha1);
	parse_tree(tree);
	init_tree_desc(&t, tree->buffer, tree->size);
	if (unpack_trees(1, &t, &opts) < 0)
		die(_("unable to checkout working tree"));

	if (write_locked_index(&the_index, lock_file, COMMIT_LOCK))
		die(_("unable to write new index file"));

	err |= run_hook_le(NULL, "post-checkout", sha1_to_hex(null_sha1),
			   sha1_to_hex(sha1), "1", NULL);

	if (!err && option_recursive)
		err = run_command_v_opt(argv_submodule, RUN_GIT_CMD);

	return err;
}

static int write_one_config(const char *key, const char *value, void *data)
{
	return git_config_set_multivar_gently(key, value ? value : "true", "^$", 0);
}

static void write_config(struct string_list *config)
{
	int i;

	for (i = 0; i < config->nr; i++) {
		if (git_config_parse_parameter(config->items[i].string,
					       write_one_config, NULL) < 0)
			die("unable to write parameters to config file");
	}
}

static void write_refspec_config(const char *src_ref_prefix,
		const struct ref *our_head_points_at,
		const struct ref *remote_head_points_at,
		struct strbuf *branch_top)
{
	struct strbuf key = STRBUF_INIT;
	struct strbuf value = STRBUF_INIT;

	if (option_mirror || !option_bare) {
		if (option_single_branch && !option_mirror) {
			if (option_branch) {
				if (starts_with(our_head_points_at->name, "refs/tags/"))
					strbuf_addf(&value, "+%s:%s", our_head_points_at->name,
						our_head_points_at->name);
				else
					strbuf_addf(&value, "+%s:%s%s", our_head_points_at->name,
						branch_top->buf, option_branch);
			} else if (remote_head_points_at) {
				const char *head = remote_head_points_at->name;
				if (!skip_prefix(head, "refs/heads/", &head))
					die("BUG: remote HEAD points at non-head?");

				strbuf_addf(&value, "+%s:%s%s", remote_head_points_at->name,
						branch_top->buf, head);
			}
			/*
			 * otherwise, the next "git fetch" will
			 * simply fetch from HEAD without updating
			 * any remote-tracking branch, which is what
			 * we want.
			 */
		} else {
			strbuf_addf(&value, "+%s*:%s*", src_ref_prefix, branch_top->buf);
		}
		/* Configure the remote */
		if (value.len) {
			strbuf_addf(&key, "remote.%s.fetch", option_origin);
			git_config_set_multivar(key.buf, value.buf, "^$", 0);
			strbuf_reset(&key);

			if (option_mirror) {
				strbuf_addf(&key, "remote.%s.mirror", option_origin);
				git_config_set(key.buf, "true");
				strbuf_reset(&key);
			}
		}
	}

	strbuf_release(&key);
	strbuf_release(&value);
}

static void dissociate_from_references(void)
{
	static const char* argv[] = { "repack", "-a", "-d", NULL };
	char *alternates = git_pathdup("objects/info/alternates");

	if (!access(alternates, F_OK)) {
		if (run_command_v_opt(argv, RUN_GIT_CMD|RUN_COMMAND_NO_STDIN))
			die(_("cannot repack to clean up"));
		if (unlink(alternates) && errno != ENOENT)
			die_errno(_("cannot unlink temporary alternates file"));
	}
	free(alternates);
}

static int do_index_pack(const char *in_pack_file, const char *out_idx_file)
{
	const char *argv[] = { "index-pack", "--clone-bundle", "-v",
			       "--check-self-contained-and-connected", "-o",
			       out_idx_file, in_pack_file, NULL };
	return run_command_v_opt(argv, RUN_GIT_CMD|RUN_COMMAND_NO_STDOUT);
}

static const char *replace_extension(const char *filename, const char *existing,
				     const char *replacement)
{
	struct strbuf new_filename = STRBUF_INIT;
	int existing_len = strlen(existing);
	int replacement_len = strlen(replacement);
	int filename_len = strlen(filename);

	if (!(filename && existing && replacement)) {
		return NULL;
	}

	if (!strncmp(filename + filename_len - existing_len,
		     existing, existing_len)) {
		int existing_position = filename_len - existing_len;
		strbuf_addstr(&new_filename, filename);
		strbuf_splice(&new_filename, existing_position, existing_len,
				replacement, replacement_len);
	}

	return strbuf_detach(&new_filename, NULL);
}

static const char *setup_and_index_pack(const char *filename)
{
	const char *primer_idx_path = NULL, *primer_bndl_path = NULL;
	primer_idx_path = replace_extension(filename, ".pack", ".idx");
	primer_bndl_path = replace_extension(filename, ".pack", ".bndl");

	if (!(primer_idx_path && primer_bndl_path)) {
		warning("invalid pack filename '%s', falling back to full "
			"clone", filename);
		return NULL;
	}

	if (!file_exists(primer_bndl_path)) {
		if (do_index_pack(filename, primer_idx_path)) {
			warning("could not index primer pack, falling back to "
				"full clone");
			return NULL;
		}
	}

	return primer_bndl_path;
}

static int write_bundle_refs(const char *bundle_filename)
{
	struct ref_transaction *t;
	struct bundle_header history_tips;
	const char *temp_ref_base = "resume";
	struct strbuf err = STRBUF_INIT;
	int i;

	init_bundle_header(&history_tips, bundle_filename);
	read_bundle_header(&history_tips);

	t = ref_transaction_begin(&err);
	for (i = 0; i < history_tips.references.nr; i++) {
		struct strbuf ref_name = STRBUF_INIT;
		strbuf_addf(&ref_name, "refs/temp/%s/%s/temp-%s",
			    option_origin, temp_ref_base,
			    sha1_to_hex(history_tips.references.list[i].sha1));
		if (!ref_exists(ref_name.buf)) {
			if (ref_transaction_create(t, ref_name.buf,
					history_tips.references.list[i].sha1,
					0, NULL, &err)) {
				warning(_("%s"), err.buf);
				return -1;
			}
		}
		strbuf_release(&ref_name);
	}

	if (initial_ref_transaction_commit(t, &err)) {
		warning("%s", err.buf);
		return -1;
	}
	ref_transaction_free(t);
	release_bundle_header(&history_tips);
	return 0;
}

static int use_alt_resource_pack(const char *alt_res_path)
{
	int ret = -1;
	const char *bundle_path = setup_and_index_pack(alt_res_path);
	if (bundle_path)
		ret = write_bundle_refs(bundle_path);
	return ret;
}

static int use_alt_resource(const char *alt_res_path)
{
	int ret = -1;
	if (!strcmp(alt_res->filetype, "pack"))
		ret = use_alt_resource_pack(alt_res_path);
	return ret;
}

static void clean_alt_resource_pack(const char *resource_path,
				    int prime_successful)
{
	struct bundle_header history_tips;
	const char *temp_ref_base = "resume";
	const char *bundle_path;

	if (!resource_path)
		return;

	bundle_path = replace_extension(resource_path, ".pack", ".bndl");

	if (prime_successful) {
		init_bundle_header(&history_tips, bundle_path);
		read_bundle_header(&history_tips);

		for (int i = 0; i < history_tips.references.nr; i++) {
			struct strbuf ref_name = STRBUF_INIT;
			strbuf_addf(&ref_name, "refs/temp/%s/%s/temp-%s",
				    option_origin, temp_ref_base,
				    sha1_to_hex(history_tips.references.list[i].sha1));
			if (ref_exists(ref_name.buf)) {
				delete_ref(ref_name.buf,
					   history_tips.references.list[i].sha1,
					   0);
			}
			strbuf_release(&ref_name);
		}
		release_bundle_header(&history_tips);
	}

	if (!prime_successful) {
		const char *tmp_path = mkpath("%s.temp", resource_path);
		const char *idx_path = replace_extension(resource_path, ".pack",
							 ".idx");
		if (file_exists(resource_path)) {
			unlink(resource_path);
		}
		if (file_exists(tmp_path)) {
			unlink(tmp_path);
		}
		if (file_exists(idx_path)) {
			unlink(idx_path);
		}
	}
	if (file_exists(bundle_path)) {
		unlink(bundle_path);
	}
}

static const char *fetch_alt_resource_pack(struct transport *transport,
					  const char *base_dir)
{
	struct strbuf download_path = STRBUF_INIT;
	const char *resource_path = NULL;
	struct remote *primer_remote = remote_get(alt_res->url);
	struct transport *primer_transport = transport_get(primer_remote,
							   alt_res->url);
	strbuf_addf(&download_path, "%s/objects/pack", base_dir);
	fprintf(stderr, "Downloading primer: %s...\n", alt_res->url);
	resource_path = transport_download_primer(primer_transport, alt_res,
						  download_path.buf);
	transport_disconnect(primer_transport);
	return resource_path;
}

static void clean_alt_resource(const char *resource_path, int prime_successful)
{
	if (!strcmp(alt_res->filetype, "pack"))
		clean_alt_resource_pack(resource_path, prime_successful);
}

static const char *fetch_alt_resource(struct transport *transport,
				      const char *base_dir)
{
	const char *resource_path = NULL;
	if (!strcmp(alt_res->filetype, "pack"))
		resource_path = fetch_alt_resource_pack(transport, base_dir);
	return resource_path;
}

static const struct alt_resource *get_last_alt_resource(void)
{
	struct alt_resource *ret = NULL;
	FILE *fp;
	if (fp = fopen(git_path_resumable(), "r")) {
		ret = xcalloc(1, sizeof(struct alt_resource));
		struct strbuf line = STRBUF_INIT;
		strbuf_getline(&line, fp);
		ret->url = strbuf_detach(&line, NULL);
		strbuf_getline(&line, fp);
		ret->filetype = strbuf_detach(&line, NULL);
		fclose(fp);
	}
	return ret;
}

struct remote_config {
	const char *name;
	const char *fetch_pattern;
	const char *worktree;
	int bare;
	int mirror;
};

static int get_remote_info(const char *key, const char *value, void *priv)
{
	struct remote_config *p = priv;
	char *sub_key;
	char *name;

	if (skip_prefix(key, "remote.", &key)) {
		name = xstrdup(key);
		sub_key = strchr(name, '.');
		*sub_key++ = 0;
		if (!p->name)
			p->name = xstrdup(name);
		else if (!strcmp(sub_key, "fetch"))
			git_config_string(&p->fetch_pattern, key, value);
		else if (!strcmp(sub_key, "mirror"))
			p->mirror =  git_config_bool(key, value);
		free(name);
	}
	else if (!strcmp(key, "core.bare"))
		p->bare =  git_config_bool(key, value);
	else if (!strcmp(key, "core.worktree"))
		git_config_string(&p->worktree, key, value);

	return 0;
}

static void get_existing_state(char *dir, const char **git_dir,
			   const char **work_tree,
			   struct remote_config *past_info)
{
	if (is_git_directory(dir)) {
		*git_dir = xstrdup(dir);
		*work_tree = NULL;
	}
	else if (file_exists(mkpath("%s/.git", dir))){
		*work_tree = xstrdup(dir);
		*git_dir = xstrdup(resolve_gitdir(mkpath("%s/.git", dir)));
	}

	if (!*git_dir)
		die(_("'%s' does not appear to be a git repo."), dir);

	set_git_dir(*git_dir);
	git_config(get_remote_info, past_info);

	if (!*work_tree) {
		if (past_info->worktree) {
			*work_tree = past_info->worktree;
		}
		else if (!past_info->bare) {
			int containing_dir_success = 1;
			char *filename = get_filename(*git_dir);
			if (filename && !strcmp(filename, ".git")) {
				const char *parent_dir = mkpath("%s/..",
								*git_dir);
				*work_tree = xstrdup(real_path(parent_dir));
				if (access(*work_tree, W_OK) < 0)
					containing_dir_success = 0;
			}
			else {
				containing_dir_success = 0;
			}
			if (!containing_dir_success)
				die(_("'%s' is configured for a work tree, "
				      "but no candidate exists."), dir);
		}
	}
	if (*work_tree)
		set_git_work_tree(*work_tree);
}

int cmd_clone(int argc, const char **argv, const char *prefix)
{
	int is_bundle = 0, is_local, argc_original, option_count;
	struct stat buf;
	const char *repo_name, *repo, *work_tree, *git_dir = NULL;
	const char *resource_path;
	char *path, *dir;
	int dest_exists;
	const struct ref *refs, *remote_head;
	const struct ref *remote_head_points_at;
	const struct ref *our_head_points_at;
	struct ref *mapped_refs;
	const struct ref *ref;
	struct strbuf key = STRBUF_INIT, value = STRBUF_INIT;
	struct strbuf branch_top = STRBUF_INIT, reflog_msg = STRBUF_INIT;
	struct transport *transport = NULL;
	const char *src_ref_prefix = "refs/heads/";
	struct remote *remote;
	int err = 0, complete_refs_before_fetch = 1;
	struct refspec *refspec;
	const char *fetch_pattern;

	packet_trace_identity("clone");
	argc_original = argc;
	argc = parse_options(argc, argv, prefix, builtin_clone_options,
			     builtin_clone_usage, 0);
	option_count = argc_original - argc;

	if (option_resume && option_count > 2) {
		die(_("--resume is incompatible with all other options."));
	}

	if (option_resume && argc != 1) {
		die(_("--resume must be specified with a single resumable "
		      "directory."));
	}

	if (argc > 2)
		usage_msg_opt(_("Too many arguments."),
			builtin_clone_usage, builtin_clone_options);

	if (argc == 0)
		usage_msg_opt(_("You must specify a repository to clone."),
			builtin_clone_usage, builtin_clone_options);

	if (option_single_branch == -1)
		option_single_branch = option_depth ? 1 : 0;

	if (option_mirror)
		option_bare = 1;

	if (option_bare) {
		if (option_origin)
			die(_("--bare and --origin %s options are incompatible."),
			    option_origin);
		if (real_git_dir)
			die(_("--bare and --separate-git-dir are incompatible."));
		option_no_checkout = 1;
	}

	if (!option_origin)
		option_origin = "origin";

	if (option_resume) {
		struct remote_config past_info = {0};
		dir = xstrdup(real_path(argv[0]));
		strip_trailing_slashes(dir);
		if (!file_exists(dir))
			die(_("directory '%s' does not exist."), dir);
		get_existing_state(dir, &git_dir, &work_tree, &past_info);

		if (!work_tree)
			option_no_checkout = 1;
		if (past_info.fetch_pattern)
			fetch_pattern = past_info.fetch_pattern;
		else {
			struct strbuf fetch_temp = STRBUF_INIT;
			strbuf_addstr(&branch_top, src_ref_prefix);
			strbuf_addf(&fetch_temp, "+%s*:%s*", src_ref_prefix,
					branch_top.buf);
			fetch_pattern = strbuf_detach(&fetch_temp, NULL);
		}

		option_origin = past_info.name;
		option_mirror = past_info.mirror;
		option_bare = past_info.bare;
		refspec = parse_fetch_refspec(1, &fetch_pattern);

		if (!(alt_res = get_last_alt_resource()))
			die(_("--resume option used, but current "
			      "directory is not resumable"));

		junk_mode = JUNK_LEAVE_RESUMABLE;
		atexit(remove_junk);
		sigchain_push_common(remove_junk_on_signal);
	}
	else {
		repo_name = argv[0];

		path = get_repo_path(repo_name, &is_bundle);
		if (path)
			repo = xstrdup(absolute_path(repo_name));
		else if (!strchr(repo_name, ':'))
			die(_("repository '%s' does not exist"), repo_name);
		else
			repo = repo_name;

		/* no need to be strict, transport_set_option() will validate it again */
		if (option_depth && atoi(option_depth) < 1)
			die(_("depth %s is not a positive number"), option_depth);

		if (argc == 2)
			dir = xstrdup(argv[1]);
		else
			dir = guess_dir_name(repo_name, is_bundle, option_bare);
		strip_trailing_slashes(dir);

		dest_exists = !stat(dir, &buf);
		if (dest_exists && !is_empty_dir(dir))
			die(_("destination path '%s' already exists and is not "
				"an empty directory."), dir);

		strbuf_addf(&reflog_msg, "clone: from %s", repo);

		if (option_bare)
			work_tree = NULL;
		else {
			work_tree = getenv("GIT_WORK_TREE");
			if (work_tree && !stat(work_tree, &buf))
				die(_("working tree '%s' already exists."), work_tree);
		}

		if (option_bare || work_tree)
			git_dir = xstrdup(dir);
		else {
			work_tree = dir;
			git_dir = mkpathdup("%s/.git", dir);
		}

		atexit(remove_junk);
		sigchain_push_common(remove_junk_on_signal);

		if (!option_bare) {
			if (safe_create_leading_directories_const(work_tree) < 0)
				die_errno(_("could not create leading directories of '%s'"),
					  work_tree);
			if (!dest_exists && mkdir(work_tree, 0777))
				die_errno(_("could not create work tree dir '%s'"),
					  work_tree);
			junk_work_tree = work_tree;
			set_git_work_tree(work_tree);
		}

		junk_git_dir = git_dir;
		if (safe_create_leading_directories_const(git_dir) < 0)
			die(_("could not create leading directories of '%s'"), git_dir);

		set_git_dir_init(git_dir, real_git_dir, 0);
		if (real_git_dir) {
			git_dir = real_git_dir;
			junk_git_dir = real_git_dir;
		}

		if (0 <= option_verbosity) {
			if (option_bare)
				fprintf(stderr, _("Cloning into bare repository '%s'...\n"), dir);
			else
				fprintf(stderr, _("Cloning into '%s'...\n"), dir);
		}
		init_db(option_template, INIT_DB_QUIET);
		write_config(&option_config);

		git_config(git_default_config, NULL);

		if (option_bare) {
			if (option_mirror)
				src_ref_prefix = "refs/";
			strbuf_addstr(&branch_top, src_ref_prefix);

			git_config_set("core.bare", "true");
		} else {
			strbuf_addf(&branch_top, "refs/remotes/%s/", option_origin);
		}

		strbuf_addf(&value, "+%s*:%s*", src_ref_prefix, branch_top.buf);
		strbuf_addf(&key, "remote.%s.url", option_origin);
		git_config_set(key.buf, repo);
		strbuf_reset(&key);

		if (option_reference.nr)
			setup_reference();

		fetch_pattern = value.buf;
		refspec = parse_fetch_refspec(1, &fetch_pattern);

		strbuf_reset(&value);
	}

	remote = remote_get(option_origin);
	transport = transport_get(remote, remote->url[0]);
	transport_set_verbosity(transport, option_verbosity, option_progress);
	transport->family = family;

	path = get_repo_path(remote->url[0], &is_bundle);
	is_local = option_local != 0 && path && !is_bundle;
	if (is_local) {
		if (option_depth)
			warning(_("--depth is ignored in local clones; use file:// instead."));
		if (!access(mkpath("%s/shallow", path), F_OK)) {
			if (option_local > 0)
				warning(_("source repository is shallow, ignoring --local"));
			is_local = 0;
		}
	}
	if (option_local > 0 && !is_local)
		warning(_("--local is ignored"));
	transport->cloning = 1;

	if (!transport->get_refs_list || (!is_local && !transport->fetch))
		die(_("Don't know how to clone %s"), transport->url);

	transport_set_option(transport, TRANS_OPT_KEEP, "yes");

	if (option_depth)
		transport_set_option(transport, TRANS_OPT_DEPTH,
				     option_depth);
	if (option_single_branch)
		transport_set_option(transport, TRANS_OPT_FOLLOWTAGS, "1");

	if (option_prime_clone)
		transport_set_option(transport, TRANS_OPT_PRIMECLONE,
				     option_prime_clone);

	if (option_upload_pack)
		transport_set_option(transport, TRANS_OPT_UPLOADPACK,
				     option_upload_pack);

	if (transport->smart_options && !option_depth)
		transport->smart_options->check_self_contained_and_connected = 1;

	if (!is_local && option_reference.nr == 0 && !alt_res)
		alt_res = transport_prime_clone(transport);
	refs = transport_get_remote_refs(transport);

	if (refs) {
		mapped_refs = wanted_peer_refs(refs, refspec);
		/*
		 * transport_get_remote_refs() may return refs with null sha-1
		 * in mapped_refs (see struct transport->get_refs_list
		 * comment). In that case we need fetch it early because
		 * remote_head code below relies on it.
		 *
		 * for normal clones, transport_get_remote_refs() should
		 * return reliable ref set, we can delay cloning until after
		 * remote HEAD check.
		 */
		for (ref = refs; ref; ref = ref->next)
			if (is_null_oid(&ref->old_oid)) {
				complete_refs_before_fetch = 0;
				break;
			}

		if (!is_local && !complete_refs_before_fetch)
			transport_fetch_refs(transport, mapped_refs);

		remote_head = find_ref_by_name(refs, "HEAD");
		remote_head_points_at =
			guess_remote_head(remote_head, mapped_refs, 0);

		if (option_branch) {
			our_head_points_at =
				find_remote_branch(mapped_refs, option_branch);

			if (!our_head_points_at)
				die(_("Remote branch %s not found in upstream %s"),
				    option_branch, option_origin);
		}
		else
			our_head_points_at = remote_head_points_at;
	}
	else {
		if (option_branch)
			die(_("Remote branch %s not found in upstream %s"),
					option_branch, option_origin);

		warning(_("You appear to have cloned an empty repository."));
		mapped_refs = NULL;
		our_head_points_at = NULL;
		remote_head_points_at = NULL;
		remote_head = NULL;
		option_no_checkout = 1;
		if (!option_bare)
			install_branch_config(0, "master", option_origin,
					      "refs/heads/master");
	}

	if (!option_resume) {
		write_refspec_config(src_ref_prefix, our_head_points_at,
				     remote_head_points_at, &branch_top);
	}

	if (alt_res) {
		junk_mode = JUNK_LEAVE_RESUMABLE;
		resource_path = fetch_alt_resource(transport, git_dir);
		if (!resource_path || use_alt_resource(resource_path) < 0) {
			if (option_resume)
				die(_("resumable resource is no longer "
				      "available or usable"));
			junk_mode = JUNK_LEAVE_NONE;
			clean_alt_resource(resource_path, 0);
			resource_path = NULL;
			alt_res = NULL;
		}
	}

	if (is_local)
		clone_local(path, git_dir);
	else if (refs && complete_refs_before_fetch)
		transport_fetch_refs(transport, mapped_refs);

	update_remote_refs(refs, mapped_refs, remote_head_points_at,
			   branch_top.buf, reflog_msg.buf, transport, !is_local);

	update_head(our_head_points_at, remote_head, reflog_msg.buf);

	transport_unlock_pack(transport);
	transport_disconnect(transport);

	if (option_dissociate) {
		close_all_packs();
		dissociate_from_references();
	}

	if (resource_path) {
		clean_alt_resource(resource_path, 1);
	}

	junk_mode = JUNK_LEAVE_REPO;
	err = checkout();

	if (file_exists(git_path_resumable())) {
		unlink(git_path_resumable());
	}

	strbuf_release(&reflog_msg);
	strbuf_release(&branch_top);
	strbuf_release(&key);
	strbuf_release(&value);
	junk_mode = JUNK_LEAVE_ALL;

	free(refspec);
	return err;
}
