/*
 * util.c -- errors, memory, strings, UTF-8, filesystem, processes.
 *
 * Everything here is plain POSIX.  No wrappers that merely rename a libc
 * call; each routine either adds a real guarantee (abort-on-OOM, recursive
 * walk, truncation check) or it does not exist.
 */
#include "ph.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int verbose = 0;

void
ph_verbose_set(int on)
{
	verbose = on;
}

/* ------------------------------------------------------------------ *
 * Diagnostics.  Everything goes to stderr: stdout belongs to the user's
 * pipeline, not to us.
 * ------------------------------------------------------------------ */
void
ph_fatal(const char *fmt, ...)
{
	va_list ap;

	fputs(PH_NAME ": fatal: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void
ph_warn(const char *fmt, ...)
{
	va_list ap;

	fputs(PH_NAME ": ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void
ph_info(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	fputs(PH_NAME ": ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ *
 * Memory.  Allocation failure is not a recoverable condition in a
 * launcher, so we do not pretend it is: abort with a clear message rather
 * than thread a NULL check through every call site.
 * ------------------------------------------------------------------ */
void *
ph_xmalloc(size_t n)
{
	void *p;

	if (n == 0)
		n = 1;
	if ((p = malloc(n)) == NULL)
		ph_fatal("out of memory (%zu bytes)", n);
	return p;
}

void *
ph_xcalloc(size_t n, size_t sz)
{
	void *p;

	if (n == 0 || sz == 0)
		n = sz = 1;
	if ((p = calloc(n, sz)) == NULL)
		ph_fatal("out of memory (%zu * %zu bytes)", n, sz);
	return p;
}

void *
ph_xrealloc(void *p, size_t n)
{
	void *q;

	if (n == 0)
		n = 1;
	if ((q = realloc(p, n)) == NULL)
		ph_fatal("out of memory (%zu bytes)", n);
	return q;
}

char *
ph_xstrdup(const char *s)
{
	size_t n;
	char *p;

	if (s == NULL)
		s = "";
	n = strlen(s) + 1;
	p = ph_xmalloc(n);
	memcpy(p, s, n);
	return p;
}

/* ------------------------------------------------------------------ *
 * strlcpy/strlcat for libcs that lack them (see ph.h).
 * ------------------------------------------------------------------ */
#ifdef PH_PROVIDE_STRL
size_t
strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t srclen = strlen(src);

	if (dstsize != 0) {
		size_t n = srclen < dstsize - 1 ? srclen : dstsize - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return srclen;		/* what we tried to make */
}

size_t
strlcat(char *dst, const char *src, size_t dstsize)
{
	size_t dlen = 0;

	while (dlen < dstsize && dst[dlen] != '\0')
		dlen++;
	if (dlen == dstsize)
		return dstsize + strlen(src);
	return dlen + strlcpy(dst + dlen, src, dstsize - dlen);
}
#endif

/* ------------------------------------------------------------------ *
 * Strings
 * ------------------------------------------------------------------ */

/*
 * Turn an arbitrary title into a filesystem-safe identifier.
 *
 * The slug is the directory name inside the library, so it must contain
 * nothing that needs quoting and nothing that can escape a directory: only
 * [a-z0-9-], never leading/trailing '-', never empty, never "." or "..".
 */
void
ph_slug(char *dst, size_t dstsize, const char *src)
{
	size_t o = 0;
	int prev_dash = 1;		/* suppress a leading dash */

	if (dstsize == 0)
		return;
	for (; *src != '\0' && o + 1 < dstsize; src++) {
		unsigned char c = (unsigned char)*src;

		if (isalnum(c)) {
			dst[o++] = (char)tolower(c);
			prev_dash = 0;
		} else if (!prev_dash) {
			dst[o++] = '-';
			prev_dash = 1;
		}
	}
	while (o > 0 && dst[o - 1] == '-')	/* no trailing dash */
		o--;
	dst[o] = '\0';
	if (dst[0] == '\0')
		strlcpy(dst, "untitled", dstsize);
}

void
ph_trim(char *s)
{
	char *p = s;
	size_t n;

	while (*p != '\0' && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

int
ph_ieq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
}

int
ph_icontains(const char *hay, const char *needle)
{
	size_t nl;

	if (needle == NULL || *needle == '\0')
		return 1;
	nl = strlen(needle);
	for (; *hay != '\0'; hay++) {
		size_t i;

		for (i = 0; i < nl; i++) {
			if (hay[i] == '\0')
				return 0;
			if (tolower((unsigned char)hay[i]) !=
			    tolower((unsigned char)needle[i]))
				break;
		}
		if (i == nl)
			return 1;
	}
	return 0;
}

void
ph_human_time(char *dst, size_t dstsize, unsigned long seconds)
{
	unsigned long h = seconds / 3600;
	unsigned long m = (seconds % 3600) / 60;

	if (seconds == 0)
		snprintf(dst, dstsize, "never");
	else if (h > 0)
		snprintf(dst, dstsize, "%luh %lum", h, m);
	else if (m > 0)
		snprintf(dst, dstsize, "%lum", m);
	else
		snprintf(dst, dstsize, "%lus", seconds);
}

void
ph_human_date(char *dst, size_t dstsize, time_t t)
{
	struct tm tmv;

	if (t <= 0) {
		strlcpy(dst, "never", dstsize);
		return;
	}
	if (localtime_r(&t, &tmv) == NULL) {
		strlcpy(dst, "?", dstsize);
		return;
	}
	if (strftime(dst, dstsize, "%Y-%m-%d %H:%M", &tmv) == 0)
		strlcpy(dst, "?", dstsize);
}

/*
 * Split a command-line-ish string into words.
 *
 * Supports '...' (literal), "..." (backslash escapes honoured) and bare
 * backslash escapes.  This is deliberately NOT a shell: no globbing, no
 * variable expansion, no command substitution.  The argv we build is handed
 * straight to execvp(3), so there is no shell to inject into -- which is
 * precisely why we do not call system(3) anywhere in this program.
 */
int
ph_argsplit(const char *s, char ***argv_out, char **store)
{
	char *buf, *w;
	char **argv;
	size_t cap = 8, n = 0;

	*argv_out = NULL;
	*store = NULL;
	if (s == NULL)
		s = "";

	buf = ph_xmalloc(strlen(s) + 1);
	argv = ph_xcalloc(cap, sizeof(*argv));
	w = buf;

	while (*s != '\0') {
		while (*s != '\0' && isspace((unsigned char)*s))
			s++;
		if (*s == '\0')
			break;
		if (n + 2 > cap) {
			cap *= 2;
			argv = ph_xrealloc(argv, cap * sizeof(*argv));
		}
		argv[n++] = w;
		while (*s != '\0' && !isspace((unsigned char)*s)) {
			if (*s == '\'') {
				for (s++; *s != '\0' && *s != '\''; s++)
					*w++ = *s;
				if (*s == '\'')
					s++;
			} else if (*s == '"') {
				for (s++; *s != '\0' && *s != '"'; s++) {
					if (*s == '\\' && s[1] != '\0')
						s++;
					*w++ = *s;
				}
				if (*s == '"')
					s++;
			} else if (*s == '\\' && s[1] != '\0') {
				s++;
				*w++ = *s++;
			} else {
				*w++ = *s++;
			}
		}
		*w++ = '\0';
	}
	argv[n] = NULL;
	*argv_out = argv;
	*store = buf;
	return (int)n;
}

/* ------------------------------------------------------------------ *
 * UTF-8
 *
 * The text engine needs code points, and the baked-in Nerd Font puts its
 * icons in the Private Use Area (U+E000..U+F8FF and the U+F0000 plane), so
 * a decoder that stops at U+FFFF is not enough.  This one is a strict
 * 4-byte decoder: overlong forms, surrogates and truncated sequences all
 * yield U+FFFD and consume exactly one byte, which keeps the caller's loop
 * from ever stalling.
 * ------------------------------------------------------------------ */
uint32_t
ph_utf8_next(const char **sp)
{
	const unsigned char *s = (const unsigned char *)*sp;
	uint32_t cp;
	int extra, i;

	if (s[0] == 0)
		return 0;
	if (s[0] < 0x80) {
		*sp += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1fu; extra = 1; }
	else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0fu; extra = 2; }
	else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 0x07u; extra = 3; }
	else { *sp += 1; return 0xfffd; }

	for (i = 1; i <= extra; i++) {
		if ((s[i] & 0xc0) != 0x80) {	/* truncated */
			*sp += 1;
			return 0xfffd;
		}
		cp = (cp << 6) | (s[i] & 0x3fu);
	}
	/* Reject overlong encodings and UTF-16 surrogates. */
	if ((extra == 1 && cp < 0x80) ||
	    (extra == 2 && cp < 0x800) ||
	    (extra == 3 && cp < 0x10000) ||
	    (cp >= 0xd800 && cp <= 0xdfff) ||
	    cp > 0x10ffff) {
		*sp += 1;
		return 0xfffd;
	}
	*sp += extra + 1;
	return cp;
}

size_t
ph_utf8_len(const char *s)
{
	size_t n = 0;

	while (ph_utf8_next(&s) != 0)
		n++;
	return n;
}

/* ------------------------------------------------------------------ *
 * Filesystem
 * ------------------------------------------------------------------ */

/*
 * Join two path components.  Returns 0 on success, -1 if the result would
 * be truncated -- and a truncated path is always an error here, never a
 * silently shortened one, because acting on half a path can delete or
 * overwrite the wrong thing.
 */
int
ph_join(char *dst, size_t dstsize, const char *a, const char *b)
{
	int need_sep;
	size_t la;

	if (dstsize == 0)
		return -1;
	if (b == NULL || *b == '\0')
		return strlcpy(dst, a, dstsize) < dstsize ? 0 : -1;
	if (*b == '/' || a == NULL || *a == '\0')
		return strlcpy(dst, b, dstsize) < dstsize ? 0 : -1;

	la = strlen(a);
	need_sep = (la > 0 && a[la - 1] != '/');
	if (strlcpy(dst, a, dstsize) >= dstsize)
		return -1;
	if (need_sep && strlcat(dst, "/", dstsize) >= dstsize)
		return -1;
	if (strlcat(dst, b, dstsize) >= dstsize)
		return -1;
	return 0;
}

int
ph_is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int
ph_is_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int
ph_is_exec(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    access(path, X_OK) == 0;
}

/* mkdir -p.  Walks the path creating each component; an already-existing
 * directory is success, not an error. */
int
ph_mkdirp(const char *path, mode_t mode)
{
	char tmp[PH_PATH_MAX];
	char *p;

	if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, mode) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int
ph_copy_file(const char *src, const char *dst)
{
	char buf[65536];
	struct stat st;
	int in, out, saved;
	ssize_t r, w, off;

	if ((in = open(src, O_RDONLY)) < 0)
		return -1;
	if (fstat(in, &st) != 0) {
		saved = errno; close(in); errno = saved; return -1;
	}
	/* Create with the source's permission bits so an executable stays
	 * executable; umask still applies, which is what we want. */
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
	if (out < 0) {
		saved = errno; close(in); errno = saved; return -1;
	}
	while ((r = read(in, buf, sizeof(buf))) > 0) {
		for (off = 0; off < r; off += w) {
			if ((w = write(out, buf + off, (size_t)(r - off))) < 0) {
				if (errno == EINTR) { w = 0; continue; }
				saved = errno;
				close(in); close(out); errno = saved;
				return -1;
			}
		}
	}
	if (r < 0) {
		saved = errno; close(in); close(out); errno = saved; return -1;
	}
	close(in);
	if (close(out) != 0)		/* deferred write errors surface here */
		return -1;
	return 0;
}

/*
 * Recursive copy.  Symlinks are recreated as symlinks rather than followed:
 * following them would both duplicate data and let a crafted source tree
 * write outside the destination.
 */
int
ph_copy_tree(const char *src, const char *dst)
{
	char sp[PH_PATH_MAX], dp[PH_PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int rc = 0;

	if (lstat(src, &st) != 0)
		return -1;
	if (S_ISLNK(st.st_mode)) {
		char link[PH_PATH_MAX];
		ssize_t n = readlink(src, link, sizeof(link) - 1);
		if (n < 0)
			return -1;
		link[n] = '\0';
		unlink(dst);
		return symlink(link, dst);
	}
	if (!S_ISDIR(st.st_mode)) {
		if (!S_ISREG(st.st_mode))
			return 0;	/* skip sockets, fifos, devices */
		return ph_copy_file(src, dst);
	}
	if (ph_mkdirp(dst, 0755) != 0)
		return -1;
	if ((d = opendir(src)) == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (ph_join(sp, sizeof(sp), src, de->d_name) != 0 ||
		    ph_join(dp, sizeof(dp), dst, de->d_name) != 0) {
			ph_warn("path too long under %s", src);
			rc = -1;
			continue;
		}
		if (ph_copy_tree(sp, dp) != 0)
			rc = -1;
	}
	closedir(d);
	return rc;
}

int
ph_rmtree(const char *path)
{
	char sp[PH_PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int rc = 0;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	if ((d = opendir(path)) == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (ph_join(sp, sizeof(sp), path, de->d_name) != 0) {
			rc = -1;
			continue;
		}
		if (ph_rmtree(sp) != 0)
			rc = -1;
	}
	closedir(d);
	if (rmdir(path) != 0)
		rc = -1;
	return rc;
}

char *
ph_read_file(const char *path, size_t *len_out)
{
	FILE *f;
	char *buf;
	long sz;
	size_t got;

	if (len_out != NULL)
		*len_out = 0;
	if ((f = fopen(path, "rb")) == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = ph_xmalloc((size_t)sz + 1);
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';		/* always NUL-terminated for text use */
	if (len_out != NULL)
		*len_out = got;
	return buf;
}

/* First existing entry from a NULL-terminated candidate list. */
int
ph_find_first(const char *dir, const char *const *names, char *out, size_t outsize)
{
	size_t i;

	for (i = 0; names[i] != NULL; i++) {
		char p[PH_PATH_MAX];

		if (ph_join(p, sizeof(p), dir, names[i]) != 0)
			continue;
		if (ph_is_file(p) || ph_is_dir(p)) {
			if (strlcpy(out, p, outsize) >= outsize)
				return -1;
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ *
 * Processes
 * ------------------------------------------------------------------ */

/*
 * fork(2) + execvp(3) + waitpid(2).  Never system(3), never popen(3):
 * both hand the string to /bin/sh, which means every game title, path and
 * argument would become shell syntax.  Here the argv array crosses into
 * the new image untouched.
 *
 * fork(2), RETURN VALUES: "Upon successful completion, fork() and _Fork()
 * return a value of 0 to the child process and return the process ID of
 * the child process to the parent process."
 *
 * The child exits with _exit(2), not exit(3): exit(3) would run atexit
 * handlers and flush stdio buffers that the child inherited from us,
 * duplicating output the parent has not written yet.
 */
int
ph_spawn(const char *cwd, char *const argv[], int logfd)
{
	pid_t pid, w;
	int status;

	if (argv == NULL || argv[0] == NULL) {
		errno = EINVAL;
		return -1;
	}
	if ((pid = fork()) < 0) {
		ph_warn("fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		if (cwd != NULL && chdir(cwd) != 0)
			_exit(126);
		if (logfd >= 0) {
			if (dup2(logfd, STDOUT_FILENO) < 0 ||
			    dup2(logfd, STDERR_FILENO) < 0)
				_exit(126);
			if (logfd > STDERR_FILENO)
				close(logfd);
		}
		execvp(argv[0], argv);
		_exit(127);		/* conventional "command not found" */
	}
	/* EINTR is expected: SDL installs signal handlers. */
	while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
		;
	if (w < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

/* Is <name> an executable somewhere on PATH? */
int
ph_have_cmd(const char *name)
{
	const char *path, *p, *e;
	char buf[PH_PATH_MAX];
	size_t n;

	if (strchr(name, '/') != NULL)
		return ph_is_exec(name);
	if ((path = getenv("PATH")) == NULL || *path == '\0')
		path = "/usr/local/bin:/usr/bin:/bin";
	for (p = path; *p != '\0'; p = (*e == ':') ? e + 1 : e) {
		e = strchr(p, ':');
		if (e == NULL)
			e = p + strlen(p);
		n = (size_t)(e - p);
		if (n == 0 || n >= sizeof(buf))
			continue;
		memcpy(buf, p, n);
		buf[n] = '\0';
		if (ph_join(buf, sizeof(buf), buf, name) == 0 && ph_is_exec(buf))
			return 1;
		if (*e == '\0')
			break;
	}
	return 0;
}
