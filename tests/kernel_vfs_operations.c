// SPDX-License-Identifier: GPL-3.0-or-later
/* Independent POSIX syscall client. Executed only inside a disposable VM. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static char root[4096];
static void fail(const char *what, int line) {
    fprintf(stderr, "OFS-POSIX-FAIL line %d: %s: %s\n", line, what, strerror(errno));
    exit(1);
}
#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)
static const char *path(const char *name) {
    static char slots[4][4096]; static unsigned at;
    char *out = slots[at++ % 4];
    CHECK(snprintf(out, 4096, "%s/%s", root, name) < 4096); return out;
}
static int make(const char *name, const char *data) {
    int fd = open(path(name), O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0);
    if (data) CHECK(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    return fd;
}
static void exact(int fd, const void *expected, size_t count, off_t at) {
    unsigned char *data = malloc(count ? count : 1); CHECK(data);
    CHECK(pread(fd, data, count, at) == (ssize_t)count);
    CHECK(!memcmp(data, expected, count)); free(data);
}
static size_t original_bytes(unsigned bs) {
    unsigned blocks = bs <= 1024 ? bs / 4 - 56 + 3 : 7;
    return (size_t)(bs - 24) * blocks + 37;
}
static unsigned char *expected(unsigned bs, size_t *length) {
    size_t first = original_bytes(bs), payload = bs - 24;
    *length = first + payload;
    unsigned char *data = calloc(1, *length); CHECK(data);
    for (size_t i = 0; i < first - 5; ++i) data[i] = (unsigned char)(i * 37 + 11);
    for (size_t i = 0; i < 17; ++i) data[payload * 3 - 2 + i] = (unsigned char)('a' + i);
    data[0] = 201; data[payload - 1] = 202; data[payload + 3] = 203;
    data[*length - 1] = 204;
    return data;
}
static void metadata(unsigned variant, int verify) {
    char name[128] = "extra/";
    unsigned count = variant == 6 ? 106 : 30;
    memset(name + 6, 'N', count); name[6 + count] = 0;
    char comment[79]; memset(comment, 'c', sizeof(comment));
    if (!verify) {
        int fd = make(name, "long name metadata"); CHECK(close(fd) == 0);
        CHECK(setxattr(path(name), "user.amiga.comment", comment, sizeof(comment), 0) == 0);
        CHECK(link(path(name), path("extra/LongAlias")) == 0);
        CHECK(setxattr(path("extra/LongAlias"), "user.amiga.comment", comment, sizeof(comment), 0) == 0);
        const char label[] = "KERNEL-QUALIFIED";
        CHECK(setxattr(root, "user.amiga.label", label, sizeof(label) - 1, 0) == 0);
        const unsigned char created[] = {0,0,0,222,0,0,1,77,0,0,1,188};
        CHECK(setxattr(root, "user.amiga.created", created, sizeof(created), 0) == 0);
    }
    char out[320];
    CHECK(getxattr(path(name), "user.amiga.comment", out, sizeof(out)) == 79);
    CHECK(!memcmp(out, comment, 79));
    CHECK(getxattr(path("extra/LongAlias"), "user.amiga.comment", out, sizeof(out)) == 79);
    CHECK(!memcmp(out, comment, 79));
    CHECK(getxattr(root, "user.amiga.label", out, sizeof(out)) == 16);
    CHECK(!memcmp(out, "KERNEL-QUALIFIED", 16));
    CHECK(listxattr(path(name), out, sizeof(out)) > 0);
}
static void lifetimes(void) {
    int old = make("extra/open", "unlinked");
    CHECK(unlink(path("extra/open")) == 0);
    struct stat st; CHECK(fstat(old, &st) == 0 && st.st_nlink == 0);
    exact(old, "unlinked", 8, 0);
    CHECK(pwrite(old, "X", 1, 1) == 1);
    CHECK(ftruncate(old, 1024) == 0);
    int fresh = make("extra/open", "new name");
    struct stat next; CHECK(fstat(fresh, &next) == 0 && next.st_ino != st.st_ino);
    exact(old, "uXlinked", 8, 0); exact(fresh, "new name", 8, 0);
    CHECK(fsync(old) == 0 && close(old) == 0);
    CHECK(close(fresh) == 0 && unlink(path("extra/open")) == 0);
    int destination = make("extra/destination", "old target");
    int source = make("extra/source", "new source"); CHECK(close(source) == 0);
    CHECK(rename(path("extra/source"), path("extra/destination")) == 0);
    exact(destination, "old target", 10, 0);
    CHECK(fstat(destination, &st) == 0 && st.st_nlink == 0);
    CHECK(pwrite(destination, "O", 1, 0) == 1);
    int replacement = open(path("extra/destination"), O_RDONLY); CHECK(replacement >= 0);
    exact(replacement, "new source", 10, 0);
    CHECK(close(replacement) == 0 && close(destination) == 0);
    CHECK(unlink(path("extra/destination")) == 0);
    CHECK(mkdir(path("extra/open-dir"), 0755) == 0);
    int dir = open(path("extra/open-dir"), O_DIRECTORY | O_RDONLY); CHECK(dir >= 0);
    CHECK(rmdir(path("extra/open-dir")) == 0);
    CHECK(fstat(dir, &st) == 0 && st.st_nlink == 0);
    CHECK(close(dir) == 0);
}
static void large_extension(unsigned bs, int verify) {
    if (bs < 2048) return;
    size_t payload = bs - 24, first = payload * (bs / 4 - 55u) + 37u;
    int fd = open(path("LargeExtension"), verify ? O_RDONLY : O_RDWR); CHECK(fd >= 0);
    size_t offset = payload * (bs / 4 - 56u) - 7u;
    const char changed[] = "extension-crossed";
    if (!verify) {
        unsigned char before[17];
        for (unsigned i = 0; i < sizeof(before); ++i) before[i] = (offset + i) & 255;
        exact(fd, before, sizeof(before), offset);
        CHECK(pwrite(fd, changed, sizeof(changed) - 1, offset) == (ssize_t)sizeof(changed) - 1);
        CHECK(pwrite(fd, "tail", 4, first + payload + 3) == 4);
        CHECK(ftruncate(fd, first + payload + 17) == 0);
        CHECK(fsync(fd) == 0);
    }
    exact(fd, changed, sizeof(changed) - 1, offset);
    unsigned char tail[17] = {0}; memcpy(tail + 3, "tail", 4);
    exact(fd, tail, sizeof(tail), first + payload);
    CHECK(close(fd) == 0);
}
static void alias_permissions(void) {
    struct stat st;
    CHECK(chmod(path("Aliased"), 0777) == 0);
    CHECK(stat(path("DirectoryAlias"), &st) == 0 && (st.st_mode & 0777) == 0777);
    CHECK(chmod(path("Aliased"), 0500) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        if (setgid(600) || setuid(600)) _exit(2);
        int fd = open(path("DirectoryAlias/Denied"), O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) { close(fd); _exit(3); }
        _exit(errno == EACCES ? 0 : 4);
    }
    int code; CHECK(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 0);
    CHECK(chmod(path("Aliased"), 0755) == 0);
}
static void create_data(unsigned bs, unsigned variant) {
    CHECK(mkdir(path("extra"), 0755) == 0);
    size_t first = original_bytes(bs), length;
    unsigned char *want = expected(bs, &length), *data = malloc(first); CHECK(data);
    for (size_t i = 0; i < first; ++i) data[i] = (unsigned char)(i * 37 + 11);
    int fd = make("extra/Data", NULL);
    CHECK(write(fd, data, first) == (ssize_t)first);
    char change[17]; for (unsigned i = 0; i < 17; ++i) change[i] = 'a' + i;
    CHECK(pwrite(fd, change, 17, (bs - 24) * 3 - 2) == 17);
    CHECK(ftruncate(fd, first + (bs - 24) * 2 + 29) == 0);
    CHECK(ftruncate(fd, first - 5) == 0);
    CHECK(ftruncate(fd, length) == 0);
    unsigned char *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapped != MAP_FAILED);
    mapped[0] = 201; mapped[bs - 25] = 202; mapped[bs - 21] = 203; mapped[length - 1] = 204;
    CHECK(msync(mapped, length, MS_SYNC) == 0 && munmap(mapped, length) == 0);
    CHECK(fsync(fd) == 0); exact(fd, want, length, 0);
    mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(mapped != MAP_FAILED); mapped[1] = 0; CHECK(munmap(mapped, length) == 0);
    exact(fd, want, length, 0);
    CHECK(link(path("extra/Data"), path("extra/Alias")) == 0);
    CHECK(unlink(path("extra/Data")) == 0); exact(fd, want, length, 0);
    CHECK(rename(path("extra/Alias"), path("extra/Data")) == 0);
    CHECK(fchmod(fd, 0640) == 0 && fchown(fd, 123, 456) == 0);
    struct timespec times[2] = {{1234567890, 400000000}, {1234567890, 400000000}};
    CHECK(futimens(fd, times) == 0 && fsync(fd) == 0 && close(fd) == 0);
    CHECK(symlink("Data", path("extra/Link")) == 0);
    CHECK(symlink("child/../Data", path("extra/ParentLink")) == 0);
    CHECK(mkdir(path("extra/child"), 0755) == 0);
    int nested = make("extra/child/File", "nested"); CHECK(close(nested) == 0);
    CHECK(mkdir(path("extra/other"), 0755) == 0);
    CHECK(rename(path("extra/child"), path("extra/other/Moved")) == 0);
    CHECK(rename(path("extra/other/Moved/File"), path("extra/Nested")) == 0);
    CHECK(rmdir(path("extra/other/Moved")) == 0 && rmdir(path("extra/other")) == 0);
    CHECK(mkdir(path("extra/child"), 0755) == 0);
    CHECK(access(path("extra/missing"), F_OK) == -1 && errno == ENOENT);
    int negative = make("extra/MISSING", "negative cache"); CHECK(close(negative) == 0);
    CHECK(access(path("extra/missing"), F_OK) == 0);
    int intl = make("extra/\xc3\xa4" "case", "international"); CHECK(close(intl) == 0);
    if (variant) CHECK(access(path("extra/\xc3\x84" "CASE"), F_OK) == 0);
    else CHECK(access(path("extra/\xc3\x84" "CASE"), F_OK) == -1 && errno == ENOENT);
    CHECK(mkdir(path("extra/entries"), 0755) == 0);
    for (unsigned i = 0; i < 64; ++i) {
        char name[64]; snprintf(name, sizeof(name), "extra/entries/slot%03u", i);
        int entry = make(name, NULL); CHECK(close(entry) == 0);
    }
    for (unsigned i = 0; i < 64; i += 2) {
        char name[64]; snprintf(name, sizeof(name), "extra/entries/slot%03u", i);
        CHECK(unlink(path(name)) == 0);
    }
    int alias = make("DirectoryAlias/from-alias", "directory alias"); CHECK(close(alias) == 0);
    alias = open(path("Aliased/from-alias"), O_RDONLY); CHECK(alias >= 0);
    exact(alias, "directory alias", 15, 0); CHECK(close(alias) == 0);
    CHECK(rename(path("Aliased/from-alias"), path("DirectoryAlias/Renamed")) == 0);
    alias_permissions();
    if (variant == 6) large_extension(bs, 0);
    metadata(variant, 0); lifetimes(); free(data); free(want);
}
static void verify_data(unsigned bs, unsigned variant) {
    size_t length; unsigned char *want = expected(bs, &length);
    int fd = open(path("extra/Data"), O_RDONLY); CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t)length && st.st_nlink == 1);
    CHECK((st.st_mode & 0777) == 0640 && st.st_uid == 123 && st.st_gid == 456);
    CHECK(st.st_mtim.tv_sec == 1234567890 && st.st_mtim.tv_nsec == 400000000);
    exact(fd, want, length, 0); CHECK(close(fd) == 0);
    fd = open(path("extra/Link"), O_RDONLY); CHECK(fd >= 0); exact(fd, want, length, 0); CHECK(close(fd) == 0);
    fd = open(path("extra/ParentLink"), O_RDONLY); CHECK(fd >= 0); exact(fd, want, length, 0); CHECK(close(fd) == 0);
    char link[128]; ssize_t n = readlink(path("extra/ParentLink"), link, sizeof(link));
    CHECK(n == 13 && !memcmp(link, "child/../Data", 13));
    CHECK(lstat(path("extra/ParentLink"), &st) == 0 && st.st_size == 13);
    fd = open(path("Aliased/Renamed"), O_RDONLY); CHECK(fd >= 0);
    exact(fd, "directory alias", 15, 0); CHECK(close(fd) == 0);
    CHECK(access(path("DirectoryAlias/from-alias"), F_OK) == -1 && errno == ENOENT);
    if (variant == 6) large_extension(bs, 1);
    DIR *directory = opendir(path("extra/entries")); CHECK(directory);
    unsigned count = 0; struct dirent *entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') ++count;
    CHECK(count == 32 && closedir(directory) == 0);
    metadata(variant, 1);
    struct statvfs fs; CHECK(statvfs(root, &fs) == 0 && fs.f_bsize == bs && fs.f_namemax == (variant == 6 ? 106 : 30));
    free(want);
}
int main(int argc, char **argv) {
    CHECK(argc == 5);
    CHECK(strlen(argv[2]) < sizeof(root)); strcpy(root, argv[2]);
    unsigned bs = strtoul(argv[3], NULL, 10), variant = strtoul(argv[4], NULL, 10);
    if (!strcmp(argv[1], "create")) create_data(bs, variant);
    else { CHECK(!strcmp(argv[1], "verify")); verify_data(bs, variant); }
    printf("OFS-POSIX-%s-PASS %u %u\n", argv[1], bs, variant);
    return 0;
}
