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

static int fail(Store *s, const char *what, const char *path)
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
    return 0;
}

static int io_fail(Store *s, const char *what, const char *path)
{
    Buf b;
    int result, err;
    err = errno;
    buf_init(&b);
    buf_puts(&b, what);
    buf_puts(&b, " (");
    buf_puts(&b, strerror(err));
    buf_putc(&b, ')');
    result = fail(s, b.s, path);
    buf_free(&b);
    return result;
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
    return p->text ? p : NULL;
}

Node *store_slot(Store *s, Ref r)
{
    size_t n, old;
    if (!store_valid_ref(s, r) || r.i > (size_t)-1 / sizeof(Node) - 1) {
        fail(s, "invalid tree reference", NULL);
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

static int kind_valid(const char *p)
{
    static const char *const kinds[] = {
        "user", "unii", "tool", "echo", "work", "note"
    };
    size_t i;
    if (!p)
        return 0;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (!strcmp(p, kinds[i]))
            return 1;
    return 0;
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

static int date_valid(const char *p, const char *filename)
{
    size_t n;
    if (!p)
        return 0;
    n = strlen(p);
    if (n != 20 && n != 25)
        return 0;
    if (!day_valid(p) || strncmp(p, filename, 10) || p[10] != 'T' ||
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

static int add_message(Store *s, const char *path, off_t offset,
                       const Json *j, const char *filename)
{
    unsigned long id, size;
    size_t n;
    const char *kind, *text, *date;
    const Json *images;
    Message *m;
    kind = json_str(json_get(j, "kind"));
    text = json_str(json_get(j, "text"));
    date = json_str(json_get(j, "date"));
    images = json_get(j, "images");
    if (!message_fields(j) || !json_ulong(json_get(j, "i"), &id) ||
        id != s->count || !kind_valid(kind) || !text || !utf8_valid(text) ||
        !json_ulong(json_get(j, "size"), &size) || size != strlen(text) ||
        !date_valid(date, filename) || s->count == ULONG_MAX)
        return fail(s, "invalid message record (id/kind/UTF-8/size/date)", path);
    if (s->count >= s->mcap) {
        n = s->mcap ? s->mcap * 2 : 64;
        if (n <= s->mcap || n > (size_t)-1 / sizeof(Message))
            return fail(s, "message index is too large", path);
        s->messages = xrealloc(s->messages, n * sizeof(Message));
        s->mcap = n;
    }
    m = s->messages + s->count;
    m->path = xstrdup(path);
    m->offset = offset;
    m->size = (size_t)size;
    m->kind = xstrdup(kind);
    m->date = xstrdup(date);
    m->has_images = images && images->child;
    s->count++;
    return 1;
}

static int add_node(Store *s, const char *path, const Json *j)
{
    unsigned long l, i, size;
    Ref r, a, b;
    Node *node;
    const char *text;
    text = json_str(json_get(j, "text"));
    if (!object_fields(j, 4) || !json_ulong(json_get(j, "l"), &l) ||
        l >= TREE_LEVELS || !json_ulong(json_get(j, "i"), &i) ||
        !text || !*text || !utf8_valid(text) ||
        !json_ulong(json_get(j, "size"), &size) || size != strlen(text))
        return fail(s, "invalid tree record (level/UTF-8/size)", path);
    r.l = (unsigned int)l;
    r.i = i;
    if (!store_valid_ref(s, r))
        return fail(s, "tree node covers nonexistent messages", path);
    if (store_node(s, r))
        return fail(s, "duplicate tree node", path);
    if (r.l) {
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        if (!store_node(s, a) || !store_node(s, b))
            return fail(s, "tree parent precedes an unbuilt child", path);
    }
    node = store_slot(s, r);
    if (!node)
        return 0;
    node->text = xstrdup(text);
    node->size = (size_t)size;
    return 1;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int load_logs(Store *s, const char *subdir, int tree)
{
    DIR *d;
    struct dirent *entry;
    char *dir, *path, **names;
    size_t count, cap, i, n;
    FILE *f;
    Buf line;
    Json *j;
    off_t offset;
    int result, rc;
    dir = path_join(s->dir, subdir);
    d = opendir(dir);
    if (!d) {
        result = io_fail(s, "cannot open log directory", dir);
        free(dir);
        return result;
    }
    names = NULL;
    count = cap = 0;
    result = 1;
    errno = 0;
    while ((entry = readdir(d)) != NULL) {
        n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 6, ".jsonl"))
            continue;
        if (n != 16 || !day_valid(entry->d_name)) {
            result = fail(s, "invalid daily log filename", entry->d_name);
            break;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            names = xrealloc(names, cap * sizeof(*names));
        }
        names[count++] = xstrdup(entry->d_name);
    }
    if (!entry && errno)
        result = io_fail(s, "cannot read log directory", dir);
    closedir(d);
    if (result && count > 1)
        qsort(names, count, sizeof(*names), name_cmp);
    buf_init(&line);
    for (i = 0; result && i < count; i++) {
        path = path_join(dir, names[i]);
        f = fopen(path, "rb");
        if (!f) {
            result = io_fail(s, "cannot read log", path);
            free(path);
            break;
        }
        for (;;) {
            offset = ftello(f);
            if (offset == (off_t)-1) {
                result = io_fail(s, "cannot index log", path);
                break;
            }
            rc = read_line(f, &line);
            if (rc == 0)
                break;
            if (rc < 0) {
                result = fail(s, "partial or unreadable log record; preserve log and recover a backup", path);
                break;
            }
            j = json_parse(line.s);
            if (!j) {
                result = fail(s, "invalid JSON log record; preserve log and recover a backup", path);
                break;
            }
            result = tree ? add_node(s, path, j) :
                     add_message(s, path, offset, j, names[i]);
            json_free(j);
            if (!result)
                break;
            if (tree)
                memcpy(s->tree_day, names[i], sizeof(s->tree_day));
        }
        fclose(f);
        free(path);
    }
    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
    free(dir);
    buf_free(&line);
    return result;
}

static void dump_view(Buf *b, const View *v)
{
    size_t i;
    buf_putc(b, '[');
    for (i = 0; i < v->n; i++) {
        if (i)
            buf_putc(b, ',');
        buf_putc(b, '[');
        buf_ulong(b, v->v[i].l);
        buf_putc(b, ',');
        buf_ulong(b, v->v[i].i);
        buf_putc(b, ']');
    }
    buf_putc(b, ']');
}

static int parse_view(Store *s, View *v, const Json *j)
{
    const Json *p;
    unsigned long l, i, end;
    Ref r;
    end = 0;
    if (!j || j->type != J_ARR)
        return fail(s, "view must be an array of [level,index] pairs", NULL);
    for (p = j->child; p; p = p->next) {
        if (p->type != J_ARR || !p->child || !p->child->next ||
            p->child->next->next || !json_ulong(p->child, &l) ||
            l >= TREE_LEVELS || !json_ulong(p->child->next, &i))
            return fail(s, "invalid saved view pair", NULL);
        r.l = (unsigned int)l;
        r.i = i;
        if (!store_valid_ref(s, r) || ref_start(r) != end ||
            (r.l && !store_node(s, r)))
            return fail(s, "saved view has a gap, overlap, or unbuilt parent", NULL);
        end += ref_span(r);
        view_add(v, r);
    }
    return 1;
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

static int atomic_file(Store *s, const char *name, const Buf *b)
{
    char *path, *tmp;
    int fd, dirfd, result, err;
    path = path_join(s->dir, name);
    tmp = path_join(s->dir, ".save-XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        result = io_fail(s, "cannot create atomic save", path);
        free(path);
        free(tmp);
        return result;
    }
    result = write_all(fd, b->s, b->len) == 0 && fsync(fd) == 0;
    err = errno;
    if (close(fd) < 0 && result) {
        result = 0;
        err = errno;
    }
    if (result && rename(tmp, path) < 0) {
        result = 0;
        err = errno;
    }
    if (result) {
        dirfd = open(s->dir, O_RDONLY);
        if (dirfd < 0) {
            result = 0;
            err = errno;
        } else {
            if (fsync(dirfd) < 0) {
                result = 0;
                err = errno;
            }
            if (close(dirfd) < 0 && result) {
                result = 0;
                err = errno;
            }
        }
    }
    if (!result) {
        unlink(tmp);
        errno = err;
        io_fail(s, "cannot save durable state", path);
    }
    free(path);
    free(tmp);
    return result;
}

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
    dump_view(&b, &s->view);
    buf_puts(&b, "}\n");
    result = atomic_file(s, ".state", &b);
    buf_free(&b);
    return result;
}

static int state_read(Store *s, int *previous, int startup)
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
        if (errno != ENOENT) {
            result = io_fail(s, "cannot read view metadata", path);
            free(path);
            return result;
        }
        *previous = s->view.shrinking;
        free(path);
        return 1;
    }
    j = json_parse(text);
    free(text);
    view_init(&intended);
    result = object_fields(j, 4) &&
             json_ulong(json_get(j, "generation"), &generation) &&
             json_ulong(json_get(j, "shrinking"), &shrinking) && shrinking <= 1 &&
             json_ulong(json_get(j, "previous_shrinking"), &prior) && prior <= 1 &&
             parse_view(s, &intended, json_get(j, "view"));
    if (!result)
        fail(s, "invalid saved view metadata", path);
    else if (startup) {
        s->generation = generation;
        s->view.shrinking = same_view(&intended, &s->view) ?
                            (int)shrinking : (int)prior;
    }
    if (result)
        *previous = (int)shrinking;
    json_free(j);
    view_free(&intended);
    free(path);
    return result;
}

int store_save_view(Store *s)
{
    Buf b;
    int previous, result;
    if (!state_read(s, &previous, 0) || !state_write(s, previous))
        return 0;
    buf_init(&b);
    dump_view(&b, &s->view);
    buf_putc(&b, '\n');
    result = atomic_file(s, "view.json", &b);
    buf_free(&b);
    return result;
}

static int acquire_lock(Store *s)
{
    char *path;
    struct stat st;
    struct flock fl;
    Lock *p;
    int result;
    path = path_join(s->dir, ".lock");
    if (stat(path, &st) == 0)
        for (p = locks; p; p = p->next)
            if (p->pid == getpid() && p->dev == st.st_dev && p->ino == st.st_ino) {
                result = fail(s, "chat already has a writer", path);
                free(path);
                return result;
            }
    s->lockfd = open(path, O_RDWR | O_CREAT, 0600);
    if (s->lockfd < 0) {
        result = io_fail(s, "cannot open chat lock", path);
        free(path);
        return result;
    }
    if (fcntl(s->lockfd, F_SETFD, FD_CLOEXEC) < 0) {
        result = io_fail(s, "cannot protect chat lock across exec", path);
        close(s->lockfd);
        s->lockfd = -1;
        free(path);
        return result;
    }
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(s->lockfd, F_SETLK, &fl) < 0) {
        result = io_fail(s, "chat already has a writer or cannot be locked", path);
        close(s->lockfd);
        s->lockfd = -1;
        free(path);
        return result;
    }
    if (fstat(s->lockfd, &st) < 0) {
        result = io_fail(s, "cannot identify chat lock", path);
        close(s->lockfd);
        s->lockfd = -1;
        free(path);
        return result;
    }
    p = xmalloc(sizeof(*p));
    p->dev = st.st_dev;
    p->ino = st.st_ino;
    p->pid = getpid();
    p->fd = s->lockfd;
    p->next = locks;
    locks = p;
    free(path);
    return 1;
}

int store_open(Store *s, const char *dir)
{
    char *path, *text;
    Json *j;
    unsigned long end;
    int result, previous, tail;
    Ref r;
    memset(s, 0, sizeof(*s));
    s->lockfd = -1;
    s->dir = xstrdup(dir);
    if (mkdirs(dir) < 0)
        return io_fail(s, "cannot create chat directory", dir);
    if (!acquire_lock(s))
        return 0;
    path = path_join(dir, "main");
    result = mkdirs(path) == 0;
    if (!result)
        io_fail(s, "cannot create message directory", path);
    free(path);
    if (!result)
        return 0;
    path = path_join(dir, "tree");
    result = mkdirs(path) == 0;
    if (!result)
        io_fail(s, "cannot create tree directory", path);
    free(path);
    if (!result || !load_logs(s, "main", 0) || !load_logs(s, "tree", 1))
        return 0;
    path = path_join(dir, "view.json");
    errno = 0;
    text = read_file(path);
    if (!text) {
        if (errno != ENOENT)
            result = io_fail(s, "cannot read saved view", path);
        else if (s->count)
            result = fail(s, "saved view is missing; restore it from backup (replay destroys the prompt cache)", path);
        else
            result = store_save_view(s);
        free(path);
        return result;
    }
    j = json_parse(text);
    free(text);
    result = parse_view(s, &s->view, j);
    json_free(j);
    free(path);
    if (!result)
        return 0;
    view_measure(s, &s->view);
    s->view.shrinking = s->view.bytes > 128000;
    if (!state_read(s, &previous, 1))
        return 0;
    if (s->view.bytes > 128000)
        s->view.shrinking = 1;
    /* Normalize the recovered prior state before another save can journal it. */
    if (!state_write(s, s->view.shrinking))
        return 0;
    end = s->view.n ? ref_start(s->view.v[s->view.n - 1]) +
                     ref_span(s->view.v[s->view.n - 1]) : 0;
    tail = end < s->count;
    for (; end < s->count; end++) {
        r.l = 0;
        r.i = end;
        view_add(&s->view, r);
    }
    view_measure(s, &s->view);
    if (s->view.bytes > 128000)
        s->view.shrinking = 1;
    /* A clean restart leaves the saved view bytes untouched. */
    return tail ? store_save_view(s) : 1;
}

void store_close(Store *s)
{
    unsigned long i;
    size_t j;
    unsigned int l;
    Lock **p, *old;
    for (i = 0; i < s->count; i++) {
        free(s->messages[i].path);
        free(s->messages[i].kind);
        free(s->messages[i].date);
    }
    free(s->messages);
    for (l = 0; l < TREE_LEVELS; l++) {
        for (j = 0; j < s->capacities[l]; j++)
            free(s->levels[l][j].text);
        free(s->levels[l]);
    }
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

static int today(Store *s, char *date, char *day)
{
    time_t now;
    struct tm t;
    const char *previous;
    now = time(NULL);
    if (now == (time_t)-1 || !gmtime_r(&now, &t) ||
        !strftime(date, 21, "%Y-%m-%dT%H:%M:%SZ", &t))
        return fail(s, "cannot obtain UTC timestamp", NULL);
    memcpy(day, date, 10);
    memcpy(day + 10, ".jsonl", 7);
    if (s->count) {
        previous = strrchr(s->messages[s->count - 1].path, '/');
        previous = previous ? previous + 1 : s->messages[s->count - 1].path;
        if (strcmp(day, previous) < 0)
            return fail(s, "clock moved to an earlier log day; correct the clock before writing", NULL);
    }
    return 1;
}

static char *daily_path(Store *s, const char *subdir, const char *day)
{
    char *dir, *path;
    dir = path_join(s->dir, subdir);
    path = path_join(dir, day);
    free(dir);
    return path;
}

static int append_line(Store *s, const char *path, const Buf *b, off_t *offset)
{
    FILE *f;
    int result, err, dirfd;
    char *dir, *slash;
    f = fopen(path, "ab");
    if (!f)
        return io_fail(s, "cannot append log", path);
    result = fseeko(f, 0, SEEK_END) == 0;
    if (result && offset) {
        *offset = ftello(f);
        result = *offset != (off_t)-1;
    }
    if (result)
        result = fwrite(b->s, 1, b->len, f) == b->len &&
                 fflush(f) == 0 && fsync(fileno(f)) == 0;
    err = errno;
    if (fclose(f) != 0 && result) {
        result = 0;
        err = errno;
    }
    if (result) {
        dir = xstrdup(path);
        slash = strrchr(dir, '/');
        if (slash)
            *slash = 0;
        dirfd = open(slash ? dir : ".", O_RDONLY);
        if (dirfd < 0) {
            result = 0;
            err = errno;
        } else {
            if (fsync(dirfd) < 0) {
                result = 0;
                err = errno;
            }
            if (close(dirfd) < 0 && result) {
                result = 0;
                err = errno;
            }
        }
        free(dir);
    }
    if (!result) {
        errno = err;
        return io_fail(s, "log write failed; stop and inspect the preserved log", path);
    }
    return 1;
}

static int append_one(Store *s, const char *kind, const char *text,
                      const Json *images)
{
    char date[21], day[17], *path;
    Buf b;
    Json *j;
    Ref r;
    off_t offset;
    int result;
    if (s->count == ULONG_MAX)
        return fail(s, "chat id space is exhausted", NULL);
    if (!today(s, date, day))
        return 0;
    path = daily_path(s, "main", day);
    buf_init(&b);
    buf_puts(&b, "{\"i\":");
    buf_ulong(&b, s->count);
    buf_puts(&b, ",\"kind\":");
    buf_json(&b, kind);
    buf_puts(&b, ",\"text\":");
    buf_json(&b, text);
    buf_puts(&b, ",\"size\":");
    buf_ulong(&b, (unsigned long)strlen(text));
    buf_puts(&b, ",\"date\":");
    buf_json(&b, date);
    if (images && images->child) {
        buf_puts(&b, ",\"images\":");
        json_dump(&b, images);
    }
    buf_puts(&b, "}\n");
    result = append_line(s, path, &b, &offset);
    if (result) {
        j = json_parse(b.s);
        result = add_message(s, path, offset, j, day);
        json_free(j);
        if (result) {
            r.l = 0;
            r.i = s->count - 1;
            view_add(&s->view, r);
            view_measure(s, &s->view);
            result = store_save_view(s);
        }
    }
    free(path);
    buf_free(&b);
    return result;
}

int store_append(Store *s, const char *kind, const char *text,
                 unsigned long *first, unsigned long *number)
{
    return store_append_images(s, kind, text, NULL, first, number);
}

int store_append_images(Store *s, const char *kind, const char *text,
                        const Json *images, unsigned long *first,
                        unsigned long *number)
{
    char *owned, *part;
    size_t remaining, n;
    unsigned long before;
    int result;
    if (!kind_valid(kind) || !text || !utf8_valid(text))
        return fail(s, "invalid message kind or UTF-8 text", NULL);
    if (images && !images_valid(images))
        return fail(s, "invalid image attachments", NULL);
    if (s->lockfd < 0 || s->count == ULONG_MAX)
        return fail(s, "chat is closed or its id space is exhausted", NULL);
    before = s->count;
    if (first)
        *first = before;
    if (number)
        *number = 0;
    owned = !strcmp(kind, "echo") ? clip_echo(text) : NULL;
    if (owned)
        text = owned;
    remaining = strlen(text);
    result = 1;
    do {
        n = remaining;
        if (!owned && n > MESSAGE_BYTES)
            n = utf8_prefix(text, MESSAGE_BYTES);
        part = xmalloc(n + 1);
        memcpy(part, text, n);
        part[n] = 0;
        result = append_one(s, kind, part, images);
        images = NULL;
        free(part);
        if (!result)
            break;
        text += n;
        remaining -= n;
    } while (remaining);
    free(owned);
    if (number)
        *number = s->count - before;
    return result;
}

int store_build(Store *s, Ref r, const char *text)
{
    char date[21], day[17], *path;
    Ref a, b;
    Node *node;
    Buf line;
    int result;
    if (!store_valid_ref(s, r) || !text || !*text || !utf8_valid(text))
        return fail(s, "invalid summary text or tree reference", NULL);
    if (store_node(s, r))
        return fail(s, "tree node was already built", NULL);
    if (r.l) {
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        if (!store_node(s, a) || !store_node(s, b))
            return fail(s, "cannot build parent before both children", NULL);
    }
    if (!today(s, date, day))
        return 0;
    if (s->tree_day[0] && strcmp(day, s->tree_day) < 0)
        return fail(s, "clock moved to an earlier tree day; correct the clock before writing", NULL);
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
    result = append_line(s, path, &line, NULL);
    if (result) {
        node = store_slot(s, r);
        result = node != NULL;
        if (node) {
            memcpy(s->tree_day, day, sizeof(s->tree_day));
            node->text = xstrdup(text);
            node->size = strlen(text);
            node->busy = 0;
            node->queued = 0;
        }
    }
    free(path);
    buf_free(&line);
    return result;
}

static Json *message_record(Store *s, unsigned long i)
{
    FILE *f;
    Buf line;
    Json *j;
    const char *text, *kind, *date;
    unsigned long stored_id, size;
    Json *result;
    const Json *images;
    if (i >= s->count) {
        fail(s, "message id does not exist", NULL);
        return NULL;
    }
    f = fopen(s->messages[i].path, "rb");
    if (!f) {
        io_fail(s, "cannot read message", s->messages[i].path);
        return NULL;
    }
    buf_init(&line);
    result = NULL;
    if (fseeko(f, s->messages[i].offset, SEEK_SET) == 0 &&
        read_line(f, &line) == 1) {
        j = json_parse(line.s);
        text = json_str(json_get(j, "text"));
        kind = json_str(json_get(j, "kind"));
        date = json_str(json_get(j, "date"));
        images = json_get(j, "images");
        if (message_fields(j) &&
            json_ulong(json_get(j, "i"), &stored_id) && stored_id == i &&
            json_ulong(json_get(j, "size"), &size) && size == s->messages[i].size &&
            kind && !strcmp(kind, s->messages[i].kind) &&
            date && !strcmp(date, s->messages[i].date) &&
            text && strlen(text) == s->messages[i].size && utf8_valid(text) &&
            s->messages[i].has_images == (images && images->child))
            result = j;
        else
            json_free(j);
    }
    if (!result)
        fail(s, "indexed message is unreadable or changed", s->messages[i].path);
    buf_free(&line);
    fclose(f);
    return result;
}

char *store_message(Store *s, unsigned long i)
{
    Json *j;
    char *text;
    j = message_record(s, i);
    if (!j)
        return NULL;
    text = xstrdup(json_str(json_get(j, "text")));
    json_free(j);
    return text;
}

char *store_images(Store *s, unsigned long i)
{
    Json *j;
    Buf b;
    if (i >= s->count) {
        fail(s, "message id does not exist", NULL);
        return NULL;
    }
    if (!s->messages[i].has_images)
        return NULL;
    j = message_record(s, i);
    if (!j)
        return NULL;
    buf_init(&b);
    json_dump(&b, json_get(j, "images"));
    json_free(j);
    return b.s;
}

const char *store_date(Store *s, unsigned long id)
{
    if (id >= s->count) {
        fail(s, "message id does not exist", NULL);
        return NULL;
    }
    return s->messages[id].date;
}

static void zoom_line(Buf *b, Ref r, const Node *node)
{
    const char *p;
    buf_ulong(b, ref_start(r));
    buf_putc(b, '+');
    buf_ulong(b, ref_span(r));
    buf_putc(b, '|');
    for (p = node->text; *p; p++)
        buf_putc(b, *p == '\n' || *p == '\r' ? ' ' : *p);
    buf_putc(b, '\n');
}

char *store_zoom(Store *s, unsigned long id, unsigned long n,
                 unsigned long page)
{
    Ref r, a, b;
    Node *left, *right;
    unsigned int l;
    unsigned long power, current, pages;
    size_t offset, chunk, length, next;
    char *text;
    Buf out;
    if (!n || (n & (n - 1)) || id % n) {
        fail(s, "zoom requires a power-of-two span and aligned id", NULL);
        return NULL;
    }
    l = 0;
    power = n;
    while (power > 1) {
        l++;
        power >>= 1;
    }
    r.l = l;
    r.i = id / n;
    if (!store_valid_ref(s, r)) {
        fail(s, "zoom range does not exist", NULL);
        return NULL;
    }
    buf_init(&out);
    if (n > 1) {
        if (page) {
            fail(s, "summary zoom has only page 0", NULL);
            buf_free(&out);
            return NULL;
        }
        a.l = b.l = r.l - 1;
        a.i = r.i * 2;
        b.i = a.i + 1;
        left = store_node(s, a);
        right = store_node(s, b);
        if (!left || !right) {
            fail(s, "zoom children are not summarized yet", NULL);
            buf_free(&out);
            return NULL;
        }
        zoom_line(&out, a, left);
        zoom_line(&out, b, right);
        return out.s;
    }
    text = store_message(s, id);
    if (!text) {
        buf_free(&out);
        return NULL;
    }
    length = strlen(text);
    offset = 0;
    pages = 0;
    do {
        next = length - offset;
        if (next > MESSAGE_BYTES)
            next = utf8_prefix(text + offset, MESSAGE_BYTES);
        offset += next;
        pages++;
    } while (offset < length);
    if (page >= pages) {
        fail(s, "zoom page does not exist (pages start at 0)", NULL);
        free(text);
        buf_free(&out);
        return NULL;
    }
    offset = 0;
    for (current = 0; current < page; current++)
        offset += utf8_prefix(text + offset, MESSAGE_BYTES);
    chunk = length - offset;
    if (chunk > MESSAGE_BYTES)
        chunk = utf8_prefix(text + offset, MESSAGE_BYTES);
    buf_puts(&out, "message ");
    buf_ulong(&out, id);
    buf_puts(&out, " kind=");
    buf_puts(&out, s->messages[id].kind);
    buf_puts(&out, " date=");
    buf_puts(&out, s->messages[id].date);
    buf_puts(&out, " page=");
    buf_ulong(&out, page);
    buf_putc(&out, '/');
    buf_ulong(&out, pages);
    buf_puts(&out, " (pages start at 0)\n");
    buf_add(&out, text + offset, chunk);
    free(text);
    return out.s;
}
