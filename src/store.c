#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* fcntl locks belong to a process, so reject another handle in this process
 * before opening the same inode (closing it would release the first lock). */
typedef struct Lock Lock;
struct Lock {
    dev_t dev;
    ino_t ino;
    pid_t pid;
    int fd;
    Lock *next;
};
static Lock *locks;

static const char *const kinds[KINDS] = {
    "user", "unii", "tool", "echo", "work", "note"
};

int store_fail(Store *s, const char *what, const char *path)
{
    Buf b;
    size_t n;
    buf_init(&b);
    buf_puts(&b, what);
    if (path) {
        buf_puts(&b, ": ");
        buf_puts(&b, path);
    }
    n = b.len;
    if (n >= sizeof(s->error))
        n = sizeof(s->error) - 1;
    memcpy(s->error, b.s, n);
    s->error[n] = 0;
    buf_free(&b);
    return -1;
}

static int io_fail(Store *s, const char *what, const char *path)
{
    Buf b;
    int err;
    err = errno;
    s->broken = 1;
    buf_init(&b);
    buf_puts(&b, what);
    buf_puts(&b, " (");
    buf_puts(&b, strerror(err));
    buf_putc(&b, ')');
    store_fail(s, b.s, path);
    buf_free(&b);
    return -1;
}

unsigned long ref_span(Ref r)
{
    return r.l < TREE_LEVELS ? 1UL << r.l : 0;
}

unsigned long ref_start(Ref r)
{
    unsigned long n;
    n = ref_span(r);
    return n && r.i <= ULONG_MAX / n ? r.i * n : 0;
}

int store_valid_ref(Store *s, Ref r)
{
    unsigned long n, start;
    n = ref_span(r);
    if (!n || r.i > ULONG_MAX / n)
        return 0;
    start = r.i * n;
    return start < s->count && n <= s->count - start;
}

Node *store_node(Store *s, Ref r)
{
    Node *p;
    if (!store_valid_ref(s, r) || r.i >= s->capacities[r.l])
        return NULL;
    p = s->levels[r.l] + r.i;
    return p->size ? p : NULL;
}

Node *store_slot(Store *s, Ref r)
{
    size_t n, old;
    if (!store_valid_ref(s, r) || r.i > (size_t)-1 / sizeof(Node) - 1) {
        store_fail(s, "invalid tree reference", NULL);
        return NULL;
    }
    old = s->capacities[r.l];
    if (r.i >= old) {
        n = old ? old : 16;
        while (n <= r.i) {
            if (n > (size_t)-1 / sizeof(Node) / 2) {
                n = (size_t)r.i + 1;
                break;
            }
            n *= 2;
        }
        s->levels[r.l] = xrealloc(s->levels[r.l], n * sizeof(Node));
        memset(s->levels[r.l] + old, 0, (n - old) * sizeof(Node));
        s->capacities[r.l] = n;
    }
    return s->levels[r.l] + r.i;
}

int store_kind_id(const char *p)
{
    int k;
    for (k = 0; p && k < KINDS; k++)
        if (!strcmp(p, kinds[k]))
            return k;
    return -1;
}

const char *store_kind(Store *s, unsigned long i)
{
    return kinds[s->messages[i].kind];
}

/* Daily log paths are interned, so an index entry names its file in 4 bytes. */
unsigned int store_file(Store *s, const char *path)
{
    unsigned int k;
    for (k = s->nfiles; k-- > 0;)
        if (!strcmp(s->files[k], path))
            return k;
    if (s->nfiles == s->fcap) {
        s->fcap = s->fcap ? s->fcap * 2 : 16;
        s->files = xrealloc(s->files, s->fcap * sizeof(*s->files));
    }
    s->files[s->nfiles] = xstrdup(path);
    return s->nfiles++;
}

static const char *base_name(const char *path)
{
    const char *slash;
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int digits(const char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    return 1;
}

static int decimal(const char *p, size_t n)
{
    int v;
    size_t i;
    v = 0;
    for (i = 0; i < n; i++)
        v = v * 10 + p[i] - '0';
    return v;
}

static int day_valid(const char *p)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31 };
    int year, month, day, limit;
    if (!digits(p, 4) || p[4] != '-' || !digits(p + 5, 2) ||
        p[7] != '-' || !digits(p + 8, 2))
        return 0;
    year = decimal(p, 4);
    month = decimal(p + 5, 2);
    day = decimal(p + 8, 2);
    if (month < 1 || month > 12 || !year)
        return 0;
    limit = days[month - 1];
    if (month == 2 && !(year % 4) && (year % 100 || !(year % 400)))
        limit++;
    return day >= 1 && day <= limit;
}

/* A record's day may precede its file's day (see today()), never follow it. */
static int date_valid(const char *p, const char *filename)
{
    size_t n;
    if (!p)
        return 0;
    n = strlen(p);
    if (n != 20 && n != 25)
        return 0;
    if (!day_valid(p) || strncmp(p, filename, 10) > 0 || p[10] != 'T' ||
        !digits(p + 11, 2) || p[13] != ':' || !digits(p + 14, 2) ||
        p[16] != ':' || !digits(p + 17, 2) ||
        decimal(p + 11, 2) > 23 || decimal(p + 14, 2) > 59 ||
        decimal(p + 17, 2) > 60)
        return 0;
    if (n == 20)
        return p[19] == 'Z';
    return (p[19] == '+' || p[19] == '-') && digits(p + 20, 2) &&
           p[22] == ':' && digits(p + 23, 2) &&
           decimal(p + 20, 2) <= 23 && decimal(p + 23, 2) <= 59;
}

static int object_fields(const Json *j, size_t n)
{
    const Json *p, *q;
    size_t count;
    if (!j || j->type != J_OBJ)
        return 0;
    count = 0;
    for (p = j->child; p; p = p->next) {
        if (!p->key)
            return 0;
        for (q = j->child; q != p; q = q->next)
            if (!strcmp(p->key, q->key))
                return 0;
        count++;
    }
    return count == n;
}

static int base64_valid(const char *s)
{
    size_t n, i, padding;
    unsigned char c;
    n = strlen(s);
    if (!n || n % 4)
        return 0;
    padding = s[n - 1] == '=' ? 1 : 0;
    if (n > 1 && s[n - 2] == '=')
        padding++;
    for (i = 0; i < n - padding; i++) {
        c = (unsigned char)s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/'))
            return 0;
    }
    for (; i < n; i++)
        if (s[i] != '=')
            return 0;
    return 1;
}

static int images_valid(const Json *images)
{
    const Json *p;
    const char *type, *data, *mime, *q;
    if (!images || images->type != J_ARR)
        return 0;
    for (p = images->child; p; p = p->next) {
        type = json_str(json_get(p, "type"));
        data = json_str(json_get(p, "data"));
        mime = json_str(json_get(p, "mimeType"));
        if (!object_fields(p, 3) || !type || strcmp(type, "image") ||
            !data || !base64_valid(data) || !mime ||
            strncmp(mime, "image/", 6) || !mime[6] || !utf8_valid(mime))
            return 0;
        for (q = mime; *q; q++)
            if ((unsigned char)*q <= 32 || (unsigned char)*q == 127)
                return 0;
    }
    return 1;
}

static int message_fields(const Json *j)
{
    const Json *images;
    images = json_get(j, "images");
    return images ? object_fields(j, 6) && images_valid(images) :
                    object_fields(j, 5);
}

/* Decoded JSON strings are valid UTF-8 without NUL, so text needs no
 * second scan. FILENAME is the record's daily file name. */
static int record_valid(const Json *j, unsigned long id, const char *filename)
{
    unsigned long stored, size;
    const char *text;
    text = json_str(json_get(j, "text"));
    return message_fields(j) && json_ulong(json_get(j, "i"), &stored) &&
           stored == id && store_kind_id(json_str(json_get(j, "kind"))) >= 0 &&
           text && json_ulong(json_get(j, "size"), &size) && size == strlen(text) &&
           date_valid(json_str(json_get(j, "date")), filename);
}

static int add_message(Store *s, unsigned int file, off_t offset, const Json *j)
{
    unsigned long size;
    size_t n;
    const Json *images;
    Message *m;
    if (s->count == ULONG_MAX || !record_valid(j, s->count, base_name(s->files[file])) ||
        !json_ulong(json_get(j, "size"), &size))
        return store_fail(s, "invalid message record (id/kind/UTF-8/size/date)", s->files[file]);
    if (s->count >= s->mcap) {
        n = s->mcap ? s->mcap * 2 : 64;
        if (n <= s->mcap || n > (size_t)-1 / sizeof(Message))
            return store_fail(s, "message index is too large", s->files[file]);
        s->messages = xrealloc(s->messages, n * sizeof(Message));
        s->mcap = n;
    }
    m = s->messages + s->count;
    m->offset = offset;
    m->size = size;
    m->file = file;
    m->kind = (unsigned char)store_kind_id(json_str(json_get(j, "kind")));
    images = json_get(j, "images");
    m->has_images = images && images->child;
    s->count++;
    return 0;
}

static int add_node(Store *s, unsigned int file, off_t offset, const Json *j)
{
    unsigned long l, i, size;
    Ref r, a, b;
    Node *node;
    const char *text;
    text = json_str(json_get(j, "text"));
    if (!object_fields(j, 4) || !json_ulong(json_get(j, "l"), &l) ||
        l >= TREE_LEVELS || !json_ulong(json_get(j, "i"), &i) || !text || !*text ||
        !json_ulong(json_get(j, "size"), &size) || size != strlen(text) || size > UINT_MAX)
        return store_fail(s, "invalid tree record (level/UTF-8/size)", s->files[file]);
    r.l = (unsigned int)l;
    r.i = i;
    if (!store_valid_ref(s, r))
        return store_fail(s, "tree node covers nonexistent messages", s->files[file]);
    if (store_node(s, r))
        return store_fail(s, "duplicate tree node", s->files[file]);
    if (r.l) {
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        if (!store_node(s, a) || !store_node(s, b))
            return store_fail(s, "tree parent precedes an unbuilt child", s->files[file]);
    }
    node = store_slot(s, r);
    if (!node)
        return -1;
    node->offset = offset;
    node->file = file;
    node->size = (unsigned int)size;
    return 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Daily log names in DIR, sorted: file order is append order. */
static int list_logs(Store *s, const char *dir, char ***names, size_t *count)
{
    DIR *d;
    struct dirent *entry;
    size_t cap, n;
    int result;
    *names = NULL;
    *count = cap = 0;
    d = opendir(dir);
    if (!d)
        return io_fail(s, "cannot open log directory", dir);
    result = 0;
    errno = 0;
    while ((entry = readdir(d)) != NULL) {
        n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 6, ".jsonl"))
            continue;
        if (n != 16 || !day_valid(entry->d_name)) {
            result = store_fail(s, "invalid daily log filename", entry->d_name);
            break;
        }
        if (*count == cap) {
            cap = cap ? cap * 2 : 16;
            *names = xrealloc(*names, cap * sizeof(**names));
        }
        (*names)[(*count)++] = xstrdup(entry->d_name);
    }
    if (!entry && errno)
        result = io_fail(s, "cannot read log directory", dir);
    closedir(d);
    if (!result && *count > 1)
        qsort(*names, *count, sizeof(**names), name_cmp);
    return result;
}

static void free_names(char **names, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

/* Streams a log's lines in large blocks. */
typedef struct Reader {
    int fd;
    off_t offset; /* file offset of block[0] */
    size_t pos, len;
    char block[65536];
} Reader;

/* 1: a complete line (its LF dropped) starting at *at; 2: a final segment
 * without LF; 0: end of file; -1: read error. */
static int reader_line(Reader *r, Buf *line, off_t *at)
{
    char *lf;
    size_t n;
    ssize_t got;
    buf_reset(line);
    *at = r->offset + (off_t)r->pos;
    for (;;) {
        if (r->pos == r->len) {
            r->offset += (off_t)r->len;
            r->pos = r->len = 0;
            do
                got = read(r->fd, r->block, sizeof(r->block));
            while (got < 0 && errno == EINTR);
            if (got < 0)
                return -1;
            if (!got)
                return line->len ? 2 : 0;
            r->len = (size_t)got;
        }
        lf = memchr(r->block + r->pos, '\n', r->len - r->pos);
        n = lf ? (size_t)(lf - (r->block + r->pos)) : r->len - r->pos;
        buf_add(line, r->block + r->pos, n);
        r->pos += n;
        if (lf) {
            r->pos++;
            buf_add(line, "", 0);
            return 1;
        }
    }
}

static int sync_path(const char *path)
{
    int fd, result, err;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    result = sync_fd(fd);
    err = errno;
    if (close(fd) < 0 && !result) {
        result = -1;
        err = errno;
    }
    errno = err;
    return result;
}

/* A new name is durable only once its directory is synced. */
static int sync_dir(const char *path)
{
    char *dir, *slash;
    int result;
    dir = xstrdup(path);
    slash = dir + strlen(dir);
    while (slash > dir + 1 && slash[-1] == '/')
        *--slash = 0; /* "chat/" names chat, not an entry inside it */
    slash = strrchr(dir, '/');
    if (slash == dir)
        slash[1] = 0; /* the parent of "/chat" is "/" */
    else if (slash)
        *slash = 0;
    result = sync_path(slash ? dir : ".");
    free(dir);
    return result;
}

/* A final record without its LF never finished its write, so its fsync never
 * returned and it was never acknowledged. The writer keeps its bytes in
 * PATH.torn (outside the *.jsonl pattern) and cuts the log back to its last
 * complete line; to a reader it is a write in progress. */
static int torn_tail(Store *s, const char *path, off_t offset, const Buf *tail)
{
    Buf torn;
    int fd, ok;
    if (s->lockfd < 0)
        return 0;
    buf_init(&torn);
    buf_puts(&torn, path);
    buf_puts(&torn, ".torn");
    fd = open(torn.s, O_WRONLY | O_APPEND | O_CREAT, 0600);
    ok = fd >= 0 && write_all(fd, tail->s, tail->len) == 0 && sync_fd(fd) == 0;
    if (fd >= 0 && close(fd) < 0)
        ok = 0;
    ok = ok && sync_dir(torn.s) == 0;
    if (ok) {
        fd = open(path, O_WRONLY);
        ok = fd >= 0 && ftruncate(fd, offset) == 0 && sync_fd(fd) == 0;
        if (fd >= 0 && close(fd) < 0)
            ok = 0;
    }
    if (ok)
        fprintf(stderr, "unii: dropped an unacknowledged torn record (%lu bytes) from %s; kept in %s\n",
                (unsigned long)tail->len, path, torn.s);
    else
        io_fail(s, "cannot set aside torn log tail", path);
    buf_free(&torn);
    return ok ? 0 : -1;
}

static int beyond(Store *s, const Json *j)
{
    unsigned long l, i;
    Ref r;
    if (!json_ulong(json_get(j, "l"), &l) || l >= TREE_LEVELS ||
        !json_ulong(json_get(j, "i"), &i))
        return 0; /* add_node reports the malformed record */
    r.l = (unsigned int)l;
    r.i = i;
    return !store_valid_ref(s, r);
}

static int load_logs(Store *s, const char *subdir, int tree)
{
    char *dir, *path, **names;
    size_t count, k;
    Reader *r;
    Buf line;
    Json *j;
    off_t at;
    unsigned int file;
    int result, rc, done;
    dir = path_join(s->dir, subdir);
    result = list_logs(s, dir, &names, &count);
    /* Appends go to the newest name, even an empty file's, so names stay
     * monotone in append order. */
    if (!result && count)
        memcpy(tree ? s->tree_day : s->main_day, names[count - 1], 17);
    r = xmalloc(sizeof(*r));
    buf_init(&line);
    done = 0;
    for (k = 0; !result && !done && k < count; k++) {
        path = path_join(dir, names[k]);
        file = store_file(s, path);
        r->fd = open(path, O_RDONLY);
        if (r->fd < 0) {
            result = io_fail(s, "cannot read log", path);
            free(path);
            break;
        }
        r->offset = 0;
        r->pos = r->len = 0;
        while (!result) {
            rc = reader_line(r, &line, &at);
            if (!rc)
                break;
            if (rc < 0) {
                result = io_fail(s, "cannot read log", path);
            } else if (rc == 2 && k + 1 == count) {
                result = torn_tail(s, path, at, &line);
                break;
            } else if (rc == 2 || memchr(line.s, 0, line.len)) {
                result = store_fail(s, "partial or unreadable log record; preserve log and recover a backup", path);
            } else if (!(j = json_parse(line.s))) {
                result = store_fail(s, "invalid JSON log record; preserve log and recover a backup", path);
            } else if (tree && s->lockfd < 0 && beyond(s, j)) {
                /* A reader read main/ first: a node over later messages was
                 * built after that snapshot, and so was everything after it. */
                json_free(j);
                done = 1;
                break;
            } else {
                result = tree ? add_node(s, file, at, j) : add_message(s, file, at, j);
                json_free(j);
            }
        }
        close(r->fd);
        free(path);
    }
    free(r);
    free_names(names, count);
    free(dir);
    buf_free(&line);
    return result;
}

static int same_view(const View *a, const View *b)
{
    size_t i;
    if (a->n != b->n)
        return 0;
    for (i = 0; i < a->n; i++)
        if (a->v[i].l != b->v[i].l || a->v[i].i != b->v[i].i)
            return 0;
    return 1;
}

int store_save_file(Store *s, const char *name, const Buf *b)
{
    char *path, *tmp;
    int fd, result, err;
    path = path_join(s->dir, name);
    tmp = path_join(s->dir, ".save-XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        io_fail(s, "cannot create atomic save", path);
        free(path);
        free(tmp);
        return -1;
    }
    result = write_all(fd, b->s, b->len) == 0 && sync_fd(fd) == 0 ? 0 : -1;
    err = errno;
    if (close(fd) < 0 && !result) {
        result = -1;
        err = errno;
    }
    if (!result && rename(tmp, path) < 0) {
        result = -1;
        err = errno;
    }
    if (!result && sync_dir(path) < 0) {
        result = -1;
        err = errno;
    }
    if (result) {
        unlink(tmp);
        errno = err;
        io_fail(s, "cannot save durable state", path);
    }
    free(path);
    free(tmp);
    return result;
}

/* .state journals view.json's batch flag: if a crash lands between the two
 * saves, the view on disk tells which flag goes with it. */
static int state_write(Store *s, int previous)
{
    Buf b;
    int result;
    buf_init(&b);
    buf_puts(&b, "{\"generation\":");
    buf_ulong(&b, s->generation);
    buf_puts(&b, ",\"shrinking\":");
    buf_ulong(&b, s->view.shrinking != 0);
    buf_puts(&b, ",\"previous_shrinking\":");
    buf_ulong(&b, previous != 0);
    buf_puts(&b, ",\"view\":");
    view_dump(&b, &s->view);
    buf_puts(&b, "}\n");
    result = store_save_file(s, ".state", &b);
    if (!result)
        s->journaled = s->view.shrinking != 0;
    buf_free(&b);
    return result;
}

static int state_read(Store *s)
{
    char *path, *text;
    Json *j;
    View intended;
    unsigned long generation, shrinking, prior;
    int result;
    path = path_join(s->dir, ".state");
    errno = 0;
    text = read_file(path);
    if (!text) {
        result = errno == ENOENT ? 0 : io_fail(s, "cannot read view metadata", path);
        free(path);
        return result;
    }
    j = json_parse(text);
    free(text);
    view_init(&intended);
    result = object_fields(j, 4) &&
             json_ulong(json_get(j, "generation"), &generation) &&
             json_ulong(json_get(j, "shrinking"), &shrinking) && shrinking <= 1 &&
             json_ulong(json_get(j, "previous_shrinking"), &prior) && prior <= 1 &&
             !view_parse(s, &intended, json_get(j, "view")) ? 0 : -1;
    if (result) {
        store_fail(s, "invalid saved view metadata", path);
    } else {
        s->generation = generation;
        s->view.shrinking = same_view(&intended, &s->view) ? (int)shrinking : (int)prior;
    }
    json_free(j);
    view_free(&intended);
    free(path);
    return result;
}

int store_save_view(Store *s)
{
    Buf b;
    int result;
    if (s->lockfd < 0)
        return store_fail(s, "chat is open read-only", NULL);
    if (state_write(s, s->journaled) < 0)
        return -1;
    buf_init(&b);
    view_dump(&b, &s->view);
    buf_putc(&b, '\n');
    result = store_save_file(s, "view.json", &b);
    buf_free(&b);
    return result;
}

static int acquire_lock(Store *s)
{
    char *path;
    struct stat st;
    struct flock fl;
    Lock *p;
    path = path_join(s->dir, ".lock");
    if (stat(path, &st) == 0)
        for (p = locks; p; p = p->next)
            if (p->pid == getpid() && p->dev == st.st_dev && p->ino == st.st_ino) {
                store_fail(s, "chat already has a writer", path);
                free(path);
                return -1;
            }
    s->lockfd = open(path, O_RDWR | O_CREAT, 0600);
    if (s->lockfd < 0) {
        io_fail(s, "cannot open chat lock", path);
        free(path);
        return -1;
    }
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(s->lockfd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(s->lockfd, F_SETLK, &fl) < 0 ||
        fstat(s->lockfd, &st) < 0) {
        io_fail(s, "chat already has a writer or cannot be locked", path);
        close(s->lockfd);
        s->lockfd = -1;
        free(path);
        return -1;
    }
    p = xmalloc(sizeof(*p));
    p->dev = st.st_dev;
    p->ino = st.st_ino;
    p->pid = getpid();
    p->fd = s->lockfd;
    p->next = locks;
    locks = p;
    free(path);
    return 0;
}

/* The log directories exist and their entries are durable, including any a
 * crashed writer created without syncing, and so is the chat directory's own
 * entry in its parent. Its stray temp files go. */
static int prepare_dir(Store *s)
{
    static const char *const subdirs[] = { "main", "tree" };
    DIR *d;
    struct dirent *entry;
    char *path;
    size_t k;
    for (k = 0; k < 2; k++) {
        path = path_join(s->dir, subdirs[k]);
        if (mkdirs(path) < 0 || sync_path(path) < 0) {
            io_fail(s, "cannot prepare log directory", path);
            free(path);
            return -1;
        }
        free(path);
    }
    if (sync_path(s->dir) < 0)
        return io_fail(s, "cannot sync chat directory", s->dir);
    if (sync_dir(s->dir) < 0)
        return io_fail(s, "cannot sync the chat directory's parent", s->dir);
    d = opendir(s->dir);
    if (!d)
        return io_fail(s, "cannot list chat directory", s->dir);
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, ".save-", 6) && strcmp(entry->d_name, ".compaction-view.tmp"))
            continue;
        path = path_join(s->dir, entry->d_name);
        unlink(path);
        free(path);
    }
    closedir(d);
    return 0;
}

/* Messages logged after the saved view join it as leaves, verbatim. */
static int add_tail(Store *s)
{
    unsigned long end;
    Ref r;
    int tail;
    end = s->view.n ? ref_start(s->view.v[s->view.n - 1]) +
                     ref_span(s->view.v[s->view.n - 1]) : 0;
    tail = end < s->count;
    for (; end < s->count; end++) {
        r.l = 0;
        r.i = end;
        view_add(&s->view, r);
    }
    view_measure(s, &s->view);
    return tail;
}

static int load_view(Store *s, const char *text)
{
    Json *j;
    int result;
    j = json_parse(text);
    result = view_parse(s, &s->view, j);
    json_free(j);
    return result;
}

int store_open(Store *s, const char *dir)
{
    char *path, *text;
    int result;
    memset(s, 0, sizeof(*s));
    s->lockfd = -1;
    s->dir = xstrdup(dir);
    if (mkdirs(dir) < 0)
        return io_fail(s, "cannot create chat directory", dir);
    if (acquire_lock(s) < 0 || prepare_dir(s) < 0 ||
        load_logs(s, "main", 0) < 0 || load_logs(s, "tree", 1) < 0)
        return -1;
    path = path_join(dir, "view.json");
    errno = 0;
    text = read_file(path);
    if (!text) {
        if (errno != ENOENT)
            result = io_fail(s, "cannot read saved view", path);
        else if (s->count)
            result = store_fail(s, "saved view is missing; restore it from backup (replay destroys the prompt cache)", path);
        else
            result = store_save_view(s);
        free(path);
        return result;
    }
    free(path);
    result = load_view(s, text);
    free(text);
    if (result < 0)
        return -1;
    view_measure(s, &s->view);
    s->view.shrinking = s->view.bytes > VIEW_HIGH;
    if (state_read(s) < 0)
        return -1;
    if (s->view.bytes > VIEW_HIGH)
        s->view.shrinking = 1;
    /* Normalize the recovered prior state before another save can journal it. */
    if (state_write(s, s->view.shrinking) < 0)
        return -1;
    /* The saved view stays as it is: appended leaves are always recovered
     * from the log, so the live view equals the one before the restart. */
    add_tail(s);
    if (s->view.bytes > VIEW_HIGH)
        s->view.shrinking = 1;
    return 0;
}

/* Readers never create, lock or write, so they work while a writer runs.
 * Writers append whole lines and rename views atomically: reading view.json
 * first, then main/, then tree/, finds everything the view names. */
int store_open_read(Store *s, const char *dir)
{
    char *path, *text;
    struct stat st;
    int result, err;
    memset(s, 0, sizeof(*s));
    s->lockfd = -1;
    s->dir = xstrdup(dir);
    path = path_join(dir, "main");
    result = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    if (!result)
        return store_fail(s, "no such chat", dir);
    path = path_join(dir, "view.json");
    errno = 0;
    text = read_file(path);
    err = errno;
    if (load_logs(s, "main", 0) < 0 || load_logs(s, "tree", 1) < 0) {
        free(text);
        free(path);
        return -1;
    }
    result = 0;
    if (!text) {
        errno = err;
        if (err != ENOENT)
            result = io_fail(s, "cannot read saved view", path);
        else if (s->count)
            result = store_fail(s, "saved view is missing; restore it from backup (replay destroys the prompt cache)", path);
        free(path);
        return result;
    }
    free(path);
    result = load_view(s, text);
    free(text);
    if (!result)
        add_tail(s);
    return result;
}

void store_close(Store *s)
{
    unsigned int k;
    Lock **p, *old;
    for (k = 0; k < s->nfiles; k++)
        free(s->files[k]);
    free(s->files);
    free(s->messages);
    for (k = 0; k < TREE_LEVELS; k++)
        free(s->levels[k]);
    view_free(&s->view);
    if (s->lockfd >= 0) {
        for (p = &locks; *p; p = &(*p)->next)
            if ((*p)->pid == getpid() && (*p)->fd == s->lockfd) {
                old = *p;
                *p = old->next;
                free(old);
                break;
            }
        close(s->lockfd);
    }
    free(s->dir);
    memset(s, 0, sizeof(*s));
    s->lockfd = -1;
}

/* The record keeps the real clock. Its file is max(newest file, today), so a
 * clock step backward (or one bad reading forward) never reorders the log
 * or stops writes; load order stays append order. */
static int today(Store *s, char *date, char *day, const char *newest)
{
    time_t now;
    struct tm t;
    now = time(NULL);
    if (now == (time_t)-1 || !gmtime_r(&now, &t) ||
        !strftime(date, 21, "%Y-%m-%dT%H:%M:%SZ", &t))
        return store_fail(s, "cannot obtain UTC timestamp", NULL);
    memcpy(day, date, 10);
    memcpy(day + 10, ".jsonl", 7);
    if (newest[0] && strcmp(day, newest) < 0)
        memcpy(day, newest, 17);
    return 0;
}

static char *daily_path(Store *s, const char *subdir, const char *day)
{
    char *dir, *path;
    dir = path_join(s->dir, subdir);
    path = path_join(dir, day);
    free(dir);
    return path;
}

/* One write and one fsync for all of B. The directory needs syncing only
 * when the file is new. */
static int append_lines(Store *s, const char *path, const Buf *b, off_t *offset)
{
    int fd, created, result, err;
    created = 0;
    fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0 && errno == ENOENT) {
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL, 0600);
        created = fd >= 0;
    }
    if (fd < 0)
        return io_fail(s, "cannot append log", path);
    *offset = lseek(fd, 0, SEEK_END);
    result = *offset >= 0 && write_all(fd, b->s, b->len) == 0 && sync_fd(fd) == 0 ? 0 : -1;
    err = errno;
    if (close(fd) < 0 && !result) {
        result = -1;
        err = errno;
    }
    if (!result && created && sync_dir(path) < 0) {
        result = -1;
        err = errno;
    }
    if (result) {
        errno = err;
        return io_fail(s, "log write failed; stop and inspect the preserved log", path);
    }
    return 0;
}

static void record(Buf *b, unsigned long id, int kind, const char *text, size_t n,
                   const char *date, const Json *images)
{
    char *part;
    part = xmalloc(n + 1);
    memcpy(part, text, n);
    part[n] = 0;
    buf_puts(b, "{\"i\":");
    buf_ulong(b, id);
    buf_puts(b, ",\"kind\":");
    buf_json(b, kinds[kind]);
    buf_puts(b, ",\"text\":");
    buf_json(b, part);
    buf_puts(b, ",\"size\":");
    buf_ulong(b, (unsigned long)n);
    buf_puts(b, ",\"date\":");
    buf_json(b, date);
    if (images && images->child) {
        buf_puts(b, ",\"images\":");
        json_dump(b, images);
    }
    buf_puts(b, "}\n");
    free(part);
}

int store_append(Store *s, const Entry *e, size_t n, unsigned long *first,
                 unsigned long *number)
{
    char date[21], day[17], *path, *clipped, *lf;
    const char *text;
    const Json *images;
    Buf b;
    Json *j;
    size_t k, rest, part, at;
    unsigned long id, before;
    off_t offset;
    unsigned int file;
    int kind, result;
    if (first)
        *first = s->count;
    if (number)
        *number = 0;
    for (k = 0; k < n; k++) {
        if (store_kind_id(e[k].kind) < 0 || !e[k].text || !utf8_valid(e[k].text))
            return store_fail(s, "invalid message kind or UTF-8 text", NULL);
        if (e[k].images && !images_valid(e[k].images))
            return store_fail(s, "invalid image attachments", NULL);
    }
    if (s->lockfd < 0)
        return store_fail(s, "chat is open read-only", NULL);
    if (today(s, date, day, s->main_day) < 0)
        return -1;
    before = id = s->count;
    buf_init(&b);
    for (k = 0; k < n; k++) {
        kind = store_kind_id(e[k].kind);
        clipped = kind == KIND_ECHO ? clip_echo(e[k].text, ECHO_CHARS) : NULL;
        text = clipped ? clipped : e[k].text;
        images = e[k].images;
        rest = strlen(text);
        do {
            part = rest;
            if (!clipped && part > MESSAGE_BYTES)
                part = utf8_prefix(text, MESSAGE_BYTES);
            if (id == ULONG_MAX) {
                free(clipped);
                buf_free(&b);
                return store_fail(s, "chat id space is exhausted", NULL);
            }
            record(&b, id++, kind, text, part, date, images);
            images = NULL;
            text += part;
            rest -= part;
        } while (rest);
        free(clipped);
    }
    path = daily_path(s, "main", day);
    result = append_lines(s, path, &b, &offset);
    if (!result) {
        memcpy(s->main_day, day, sizeof(s->main_day));
        file = store_file(s, path);
        /* Index what was written, validated exactly as a load would. */
        for (at = 0; !result && at < b.len; at = (size_t)(lf - b.s) + 1) {
            lf = memchr(b.s + at, '\n', b.len - at);
            *lf = 0;
            j = json_parse(b.s + at);
            result = add_message(s, file, offset + (off_t)at, j);
            json_free(j);
            if (!result) {
                Ref r;
                r.l = 0;
                r.i = s->count - 1;
                view_add(&s->view, r);
            }
        }
        if (result)
            s->broken = 1;
        view_measure(s, &s->view);
    }
    if (number)
        *number = s->count - before;
    free(path);
    buf_free(&b);
    return result;
}

int store_build(Store *s, Ref r, const char *text)
{
    char date[21], day[17], *path;
    Ref a, b;
    Node *node;
    Buf line;
    off_t offset;
    int result;
    if (s->lockfd < 0)
        return store_fail(s, "chat is open read-only", NULL);
    if (!store_valid_ref(s, r) || !text || !*text || !utf8_valid(text) ||
        strlen(text) > UINT_MAX)
        return store_fail(s, "invalid summary text or tree reference", NULL);
    if (store_node(s, r))
        return store_fail(s, "tree node was already built", NULL);
    if (r.l) {
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        if (!store_node(s, a) || !store_node(s, b))
            return store_fail(s, "cannot build parent before both children", NULL);
    }
    if (today(s, date, day, s->tree_day) < 0)
        return -1;
    path = daily_path(s, "tree", day);
    buf_init(&line);
    buf_puts(&line, "{\"l\":");
    buf_ulong(&line, r.l);
    buf_puts(&line, ",\"i\":");
    buf_ulong(&line, r.i);
    buf_puts(&line, ",\"text\":");
    buf_json(&line, text);
    buf_puts(&line, ",\"size\":");
    buf_ulong(&line, (unsigned long)strlen(text));
    buf_puts(&line, "}\n");
    result = append_lines(s, path, &line, &offset);
    if (!result) {
        memcpy(s->tree_day, day, sizeof(s->tree_day));
        node = store_slot(s, r);
        node->offset = offset;
        node->file = store_file(s, path);
        node->size = (unsigned int)strlen(text);
        node->busy = 0;
        node->queued = NODE_IDLE;
    }
    free(path);
    buf_free(&line);
    return result;
}

/* The complete line at OFFSET of an indexed log. */
static int line_at(Store *s, unsigned int file, off_t offset, Buf *line)
{
    char chunk[8192], *lf;
    ssize_t got;
    int fd, err;
    buf_reset(line);
    fd = open(s->files[file], O_RDONLY);
    if (fd < 0)
        return io_fail(s, "cannot read log", s->files[file]);
    for (;;) {
        got = pread(fd, chunk, sizeof(chunk), offset + (off_t)line->len);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        lf = memchr(chunk, '\n', (size_t)got);
        buf_add(line, chunk, lf ? (size_t)(lf - chunk) : (size_t)got);
        if (lf) {
            close(fd);
            return memchr(line->s, 0, line->len) ? -1 : 0;
        }
    }
    err = errno;
    close(fd);
    if (got < 0) {
        errno = err;
        return io_fail(s, "cannot read log", s->files[file]);
    }
    return -1; /* the caller reports a changed log */
}

Json *store_record(Store *s, unsigned long i)
{
    Buf line;
    Json *j;
    const Json *images;
    const Message *m;
    unsigned long size;
    if (i >= s->count) {
        store_fail(s, "message id does not exist", NULL);
        return NULL;
    }
    m = s->messages + i;
    buf_init(&line);
    j = line_at(s, m->file, m->offset, &line) ? NULL : json_parse(line.s);
    images = json_get(j, "images");
    if (j && !(record_valid(j, i, base_name(s->files[m->file])) &&
               json_ulong(json_get(j, "size"), &size) && size == m->size &&
               store_kind_id(json_str(json_get(j, "kind"))) == m->kind &&
               m->has_images == (images && images->child))) {
        json_free(j);
        j = NULL;
    }
    buf_free(&line);
    if (!j && !s->broken) {
        s->broken = 1;
        store_fail(s, "indexed message is unreadable or changed", s->files[m->file]);
    }
    return j;
}

char *store_message(Store *s, unsigned long i)
{
    Json *j;
    char *text;
    j = store_record(s, i);
    if (!j)
        return NULL;
    text = xstrdup(json_str(json_get(j, "text")));
    json_free(j);
    return text;
}

char *store_date(Store *s, unsigned long i)
{
    Json *j;
    char *date;
    j = store_record(s, i);
    if (!j)
        return NULL;
    date = xstrdup(json_str(json_get(j, "date")));
    json_free(j);
    return date;
}

char *store_images(Store *s, unsigned long i)
{
    Json *j;
    Buf b;
    if (i >= s->count) {
        store_fail(s, "message id does not exist", NULL);
        return NULL;
    }
    if (!s->messages[i].has_images)
        return NULL;
    j = store_record(s, i);
    if (!j)
        return NULL;
    buf_init(&b);
    json_dump(&b, json_get(j, "images"));
    json_free(j);
    return b.s;
}

char *store_text(Store *s, Ref r)
{
    Node *n;
    Buf line;
    Json *j;
    const char *text;
    unsigned long l, i, size;
    char *result;
    n = store_node(s, r);
    if (!n) {
        store_fail(s, "tree node is not built", NULL);
        return NULL;
    }
    buf_init(&line);
    j = line_at(s, n->file, n->offset, &line) ? NULL : json_parse(line.s);
    text = json_str(json_get(j, "text"));
    result = j && object_fields(j, 4) && json_ulong(json_get(j, "l"), &l) && l == r.l &&
             json_ulong(json_get(j, "i"), &i) && i == r.i && text &&
             json_ulong(json_get(j, "size"), &size) && size == n->size &&
             strlen(text) == size ? xstrdup(text) : NULL;
    json_free(j);
    buf_free(&line);
    if (!result && !s->broken) {
        s->broken = 1;
        store_fail(s, "indexed tree node is unreadable or changed", s->files[n->file]);
    }
    return result;
}

/* Pages split at code points. Returns the page count and page P's span. */
static unsigned long page_span(const char *text, size_t length, unsigned long p,
                               size_t *offset, size_t *chunk)
{
    size_t at, next;
    unsigned long pages;
    at = *offset = *chunk = 0;
    pages = 0;
    do {
        next = length - at;
        if (next > MESSAGE_BYTES)
            next = utf8_prefix(text + at, MESSAGE_BYTES);
        if (pages == p) {
            *offset = at;
            *chunk = next;
        }
        at += next;
        pages++;
    } while (at < length);
    return pages;
}

/* A child line's head inside a log reference, newlines as spaces. */
static void line_head(Buf *b, Ref r, const char *text)
{
    size_t n, k;
    n = utf8_prefix(text, ZOOM_HEAD);
    buf_ulong(b, ref_start(r));
    buf_putc(b, '+');
    buf_ulong(b, ref_span(r));
    buf_putc(b, '|');
    for (k = 0; k < n; k++)
        buf_putc(b, text[k] == '\n' || text[k] == '\r' ? ' ' : text[k]);
    if (text[n])
        buf_puts(b, "...");
}

char *store_zoom(Store *s, unsigned long id, unsigned long n,
                 unsigned long page, char **ref)
{
    Ref r, a, b;
    unsigned int l;
    unsigned long power, pages;
    size_t offset, chunk;
    const char *text;
    char *left, *right;
    Json *j;
    Buf out, note;
    if (ref)
        *ref = NULL;
    if (!n || (n & (n - 1)) || id % n) {
        store_fail(s, "zoom requires a power-of-two span and aligned id", NULL);
        return NULL;
    }
    l = 0;
    for (power = n; power > 1; power >>= 1)
        l++;
    r.l = l;
    r.i = id / n;
    if (!store_valid_ref(s, r)) {
        store_fail(s, "zoom range does not exist", NULL);
        return NULL;
    }
    buf_init(&out);
    buf_init(&note);
    if (n > 1) {
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        if (page) {
            store_fail(s, "summary zoom has only page 0", NULL);
            return NULL;
        }
        if (!store_node(s, a) || !store_node(s, b)) {
            store_fail(s, "zoom children are not summarized yet", NULL);
            return NULL;
        }
        left = store_text(s, a);
        right = left ? store_text(s, b) : NULL;
        if (!right) {
            free(left);
            return NULL;
        }
        view_line(&out, a, left);
        view_line(&out, b, right);
        buf_puts(&note, "lines ");
        line_head(&note, a, left);
        buf_puts(&note, "; ");
        line_head(&note, b, right);
        free(left);
        free(right);
    } else {
        j = store_record(s, id);
        if (!j)
            return NULL;
        text = json_str(json_get(j, "text"));
        pages = page_span(text, strlen(text), page, &offset, &chunk);
        if (page >= pages) {
            store_fail(s, "zoom page does not exist (pages start at 0)", NULL);
            json_free(j);
            return NULL;
        }
        buf_puts(&out, "message ");
        buf_ulong(&out, id);
        buf_puts(&out, " kind=");
        buf_puts(&out, store_kind(s, id));
        buf_puts(&out, " date=");
        buf_puts(&out, json_str(json_get(j, "date")));
        buf_puts(&out, " page=");
        buf_ulong(&out, page);
        buf_putc(&out, '/');
        buf_ulong(&out, pages);
        buf_puts(&out, " (pages start at 0)\n");
        buf_add(&out, text + offset, chunk);
        json_free(j);
        buf_puts(&note, "message ");
        buf_ulong(&note, id);
        buf_puts(&note, " page ");
        buf_ulong(&note, page);
        buf_putc(&note, '/');
        buf_ulong(&note, pages);
        buf_puts(&note, ", ");
        buf_ulong(&note, (unsigned long)chunk);
        buf_puts(&note, " bytes");
    }
    buf_puts(&note, "; not copied; zoom again to read");
    if (ref)
        *ref = note.s;
    else
        buf_free(&note);
    return out.s;
}

/* The writer's records begin {"i":N, so skipping needs no full parse. */
static int record_id(const char *line, unsigned long *id)
{
    const char *p;
    unsigned long n, d;
    Json *j;
    int ok;
    p = line + 5;
    if (!strncmp(line, "{\"i\":", 5) && *p >= '0' && *p <= '9' &&
        (*p != '0' || p[1] == ',')) {
        for (n = 0; *p >= '0' && *p <= '9'; p++) {
            d = (unsigned long)(*p - '0');
            if (n > (ULONG_MAX - d) / 10)
                return 0;
            n = n * 10 + d;
        }
        if (*p == ',') {
            *id = n;
            return 1;
        }
    }
    j = json_parse(line);
    ok = json_ulong(json_get(j, "i"), id);
    json_free(j);
    return ok;
}

static int open_reader(Store *s, Reader *r, const char *path)
{
    r->fd = open(path, O_RDONLY);
    r->offset = 0;
    r->pos = r->len = 0;
    return r->fd < 0 ? io_fail(s, "cannot read log", path) : 0;
}

/* First record id of a daily log; *found is 0 if it has no complete line. */
static int first_id(Store *s, Reader *r, Buf *line, const char *path,
                    unsigned long *id, int *found)
{
    off_t at;
    int rc, result;
    *found = 0;
    if (open_reader(s, r, path) < 0)
        return -1;
    rc = reader_line(r, line, &at);
    result = 0;
    if (rc < 0)
        result = io_fail(s, "cannot read log", path);
    else if (rc == 1 && !(*found = record_id(line->s, id)))
        result = store_fail(s, "invalid log record", path);
    close(r->fd);
    return result;
}

int store_log(Store *s, const char *dir, unsigned long from, unsigned long count,
              FILE *out)
{
    char *logs, *path, **names;
    size_t n, k, start;
    Reader *r;
    Buf line;
    Json *j;
    unsigned long id, next, end;
    off_t at;
    int result, rc, found;
    memset(s, 0, sizeof(*s));
    s->lockfd = -1;
    s->dir = xstrdup(dir);
    end = count > ULONG_MAX - from ? ULONG_MAX : from + count;
    logs = path_join(dir, "main");
    result = list_logs(s, logs, &names, &n);
    r = xmalloc(sizeof(*r));
    buf_init(&line);
    /* Newest first: recent pages open one or two files. */
    start = 0;
    for (k = n; !result && k-- > 0;) {
        path = path_join(logs, names[k]);
        result = first_id(s, r, &line, path, &id, &found);
        free(path);
        if (!result && found && id <= from) {
            start = k;
            break;
        }
    }
    next = from;
    for (k = start; !result && next < end && k < n; k++) {
        path = path_join(logs, names[k]);
        if (open_reader(s, r, path) < 0) {
            free(path);
            result = -1;
            break;
        }
        while (!result && next < end) {
            rc = reader_line(r, &line, &at);
            if (!rc || (rc == 2 && k + 1 == n))
                break; /* the last line may still be being written */
            if (rc < 0) {
                result = io_fail(s, "cannot read log", path);
            } else if (rc == 2 || memchr(line.s, 0, line.len) || !record_id(line.s, &id)) {
                result = store_fail(s, "partial or invalid log record", path);
            } else if (id >= from) {
                j = json_parse(line.s);
                if (id != next || !j || !record_valid(j, id, names[k]))
                    result = store_fail(s, "invalid or out-of-order log record", path);
                else if (fputs(line.s, out) == EOF || putc('\n', out) == EOF)
                    result = io_fail(s, "cannot write records", NULL);
                else
                    next++;
                json_free(j);
            }
        }
        close(r->fd);
        free(path);
    }
    if (!result && fflush(out) == EOF)
        result = io_fail(s, "cannot write records", NULL);
    free(r);
    free_names(names, n);
    free(logs);
    buf_free(&line);
    return result;
}
