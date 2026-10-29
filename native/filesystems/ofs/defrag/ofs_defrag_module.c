/* SPDX-License-Identifier: GPL-3.0-or-later
 * OFS-owned command, placement and recovery contract. Host services bind one
 * opened target to a stable identity/capacity, refuse mounted overlap, provide
 * exclusive source writes and generic durable files/crypto. They interpret no
 * OFS metadata or transaction phase. No process/libc imports enter OFS.so.
 */
#include "ofs_relayout.h"

#define JOURNAL_MAGIC "IFS-DEFRAG-JOURNAL:1:ofs\n"
#define JOURNAL_BYTES 12288u
typedef struct {
    char device[IFS_DEFRAG_PATH_BYTES], identity[IFS_DEFRAG_ID_BYTES];
    char stage[IFS_DEFRAG_PATH_BYTES], token[65], source_hash[65], stage_hash[65];
    uint64_t bytes;
    uint32_t block_bytes, blocks, reserved, root, variant, growth, phase;
} Journal;
static size_t length(const char *s) { size_t n = 0; if (s) while (s[n]) ++n; return n; }
static int equal(const char *a, const char *b) {
    size_t n = length(a); return a && b && n == length(b) && ofs_equal(a, b, n);
}
static int copy(char *out, size_t bytes, const char *s) {
    size_t n = length(s); if (!s || n >= bytes) return IFS_OFS_INVALID;
    ofs_copy(out, s, n + 1u); return 0;
}
static int safe(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; ++s) if (*s == '\n' || *s == '\r' || *s == '=') return 0;
    return 1;
}
static int stage_path(const IfsDefragHostV2 *h, const char *journal, char *out) {
    int n = h->format(out, IFS_DEFRAG_PATH_BYTES, "%s.ofs-stage", journal);
    return n < 0 || (uint32_t)n >= IFS_DEFRAG_PATH_BYTES ? IFS_OFS_INVALID : 0;
}
static int save(const IfsDefragHostV2 *h, const char *path, Journal *j, uint32_t phase) {
    char *text = h->allocate(JOURNAL_BYTES); if (!text) return IFS_OFS_NO_MEMORY;
    j->phase = phase;
    int n = h->format(text, JOURNAL_BYTES, JOURNAL_MAGIC
        "device=%s\nidentity=%s\nstage=%s\nbytes=%llu\nblock_bytes=%u\nblocks=%u\n"
        "reserved=%u\nroot=%u\nvariant=%u\ngrowth=%u\nphase=%u\n"
        "token=%s\nsource_hash=%s\nstage_hash=%s\n",
        j->device, j->identity, j->stage, (unsigned long long)j->bytes, j->block_bytes,
        j->blocks, j->reserved, j->root, j->variant, j->growth, j->phase,
        j->token, j->source_hash, j->stage_hash);
    int r = n < 0 || (uint32_t)n >= JOURNAL_BYTES ? IFS_OFS_INVALID :
            h->atomic_file(h->context, path, text, (uint32_t)n) ? IFS_OFS_IO_ERROR : 0;
    h->release(text); return r;
}
static int number(const char *s, uint64_t *out) {
    uint64_t value = 0; if (!s || !*s) return IFS_OFS_INVALID;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || value > (UINT64_MAX - (unsigned)(*s-'0')) / 10u)
            return IFS_OFS_INVALID;
        value = value * 10u + (unsigned)(*s-'0');
    }
    *out = value; return 0;
}
static int hash_valid(const char *s) {
    if (length(s) != 64u) return 0;
    for (unsigned i = 0; i < 64u; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
static int load(const IfsDefragHostV2 *h, const char *path, Journal *j) {
    static const char *keys[] = {"device", "identity", "stage", "bytes", "block_bytes", "blocks",
        "reserved", "root", "variant", "growth", "phase", "token", "source_hash", "stage_hash"};
    char *text = h->allocate(JOURNAL_BYTES); if (!text) return IFS_OFS_NO_MEMORY;
    uint32_t bytes = 0; int r = h->read_file(h->context, path, text, JOURNAL_BYTES - 1u, &bytes);
    size_t magic = length(JOURNAL_MAGIC); uint32_t seen = 0;
    if (r || bytes < magic || !ofs_equal(text, JOURNAL_MAGIC, magic)) r = IFS_OFS_INVALID;
    if (!r) {
        text[bytes] = 0; ofs_zero(j, sizeof(*j)); char *at = text + magic;
        while (*at && !r) {
            char *key = at, *value = 0;
            while (*at && *at != '\n') { if (*at == '=' && !value) { *at = 0; value = at + 1; } ++at; }
            if (!*at || !value) { r = IFS_OFS_INVALID; break; }
            *at++ = 0; unsigned index = 0;
            while (index < 14u && !equal(key, keys[index])) ++index;
            if (index == 14u || (seen & (1u << index))) { r = IFS_OFS_INVALID; break; }
            seen |= 1u << index;
            if (index < 3u || index >= 11u) {
                char *out = index == 0 ? j->device : index == 1 ? j->identity : index == 2 ? j->stage :
                            index == 11 ? j->token : index == 12 ? j->source_hash : j->stage_hash;
                size_t cap = index == 1 ? sizeof(j->identity) : index < 3 ? sizeof(j->device) : 65u;
                r = copy(out, cap, value);
            } else {
                uint64_t n; r = number(value, &n);
                if (!r && index != 3u && n > UINT32_MAX) r = IFS_OFS_INVALID;
                if (r) break;
                switch (index) {
                case 3: j->bytes = n; break;
                case 4: j->block_bytes = (uint32_t)n; break;
                case 5: j->blocks = (uint32_t)n; break;
                case 6: j->reserved = (uint32_t)n; break;
                case 7: j->root = (uint32_t)n; break;
                case 8: j->variant = (uint32_t)n; break;
                case 9: j->growth = (uint32_t)n; break;
                case 10: j->phase = (uint32_t)n; break;
                default: r = IFS_OFS_INVALID; break;
                }
            }
        }
    }
    if (!r && (seen != ((1u << 14u) - 1u) || !safe(j->device) || !safe(j->identity) ||
        !safe(j->stage) || !j->bytes || j->growth > 1u || j->phase < 1u || j->phase > 3u ||
        !hash_valid(j->token) || !hash_valid(j->source_hash) || !hash_valid(j->stage_hash) ||
        (j->variant != 0u && j->variant != 2u && j->variant != 4u && j->variant != 6u))) r = IFS_OFS_INVALID;
    h->release(text); return r;
}
static int token(const IfsDefragHostV2 *h, IfsDefragTarget *t, const Journal *j, char out[65]) {
    return ofs_layout_token(h, t, j->block_bytes, j->blocks, j->reserved, j->root, j->variant, out);
}
static int cleanup(const IfsDefragHostV2 *h, const char *path, const Journal *j) {
    /* If removing the journal fails, retain the stage needed by Recover. */
    if (h->remove_file(h->context, path)) return IFS_OFS_IO_ERROR;
    return h->remove_file(h->context, j->stage) ? IFS_OFS_IO_ERROR : 0;
}
static void result(const IfsDefragHostV2 *h, const char *operation, int status) {
    h->print(h->context, 0, "@@RESULT {\"operation\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}\n",
        operation, status == 0 ? "completed" : status == IFS_DEFRAG_STOPPED ? "stopped" : "failed",
        status == IFS_DEFRAG_STOPPED ? "Durable recovery state is retained when source writes began." : "");
}
static int host_valid(const IfsDefragHostV2 *h) {
    return h && h->abi_version == IFS_DEFRAG_ABI_V2 && h->structure_bytes >= sizeof(*h) &&
        h->program_version && h->allocate && h->release && h->open_target && h->create_stage &&
        h->close_target && h->read_at && h->write_at && h->sync_target && h->file_exists &&
        h->read_file && h->atomic_file && h->remove_file && h->digest_create && h->digest_update &&
        h->digest_finish && h->digest_release && h->stop_requested && h->print && h->format;
}
static int run(const IfsDefragHostV2 *h, int argc, char **argv) {
    if (!host_valid(h) || argc < 3 || !argv) return 1;
    const char *mode = argv[1], *path = argv[2], *confirm = 0, *journal_path = 0;
    int growth = equal(mode, "growth-defrag"), recover = equal(mode, "recover"), write = 0, live = 0;
    int analyse = equal(mode, "analyse-json"), identify = equal(mode, "identify");
    if (!growth && !recover && !analyse && !identify && !equal(mode, "defrag")) return 2;
    for (int i = 3; i < argc; ++i) {
        if (equal(argv[i], "--write")) write = 1;
        else if (equal(argv[i], "--live-updates")) live = 1;
        else if (equal(argv[i], "--confirm") && i + 1 < argc) confirm = argv[++i];
        else if (equal(argv[i], "--journal") && i + 1 < argc) journal_path = argv[++i];
        else if (equal(argv[i], "--growth-percent") && i + 1 < argc) {
            if (!equal(argv[++i], "10")) return 2;
        } else if ((equal(argv[i], "--workers") || equal(argv[i], "--ram-buffer") ||
                    equal(argv[i], "--batch-clusters") || equal(argv[i], "--live-map-cells")) && i + 1 < argc) ++i;
        else return 2;
    }
    if (!analyse && !identify && (!write || !equal(confirm, path) || !safe(journal_path))) {
        h->print(h->context, 1, "OFS mutation requires --write --confirm DEVICE --journal PATH\n"); return 2;
    }
    h->print(h->context, 1, "[filesystem-module] ofs defrag ABI 2: authoritative OFS.so\n");
    IfsDefragTarget source = {0}, stage = {0}, target = {0};
    OfsLayout original = {0}, staged = {0}; Journal *j = h->allocate(sizeof(*j));
    if (!j) return 1;
    ofs_zero(j, sizeof(*j)); int r = 0, journal_saved = 0, stage_created = 0;
    if (recover) {
        r = load(h, journal_path, j); char derived[IFS_DEFRAG_PATH_BYTES];
        if (!r) r = stage_path(h, journal_path, derived);
        if (!r && !equal(j->stage, derived)) r = IFS_OFS_INVALID;
        if (!r && h->open_target(h->context, path, 1, j->identity, j->bytes, &target)) r = IFS_OFS_IO_ERROR;
        if (!r && !equal(target.path, j->device)) r = IFS_OFS_INVALID;
        char digest[65];
        if (!r) r = token(h, &target, j, digest);
        if (!r && !equal(digest, j->token)) r = IFS_OFS_CORRUPT;
        /* Phase 1 is published before any source write. Unlike an interrupted
         * commit, its source must still match the complete original used map.
         * A same-inode edit after staging must not be overwritten by Recover. */
        if (!r && j->phase == 1u) {
            r = ofs_layout_open(&original, h, &target);
            if (!r) r = ofs_layout_source_hash(&original, &target, digest);
            if (!r && !equal(digest, j->source_hash)) r = IFS_OFS_CORRUPT;
        }
        if (!r && h->open_target(h->context, j->stage, 0, 0, j->bytes, &stage)) r = IFS_OFS_IO_ERROR;
        if (!r) r = ofs_layout_open(&staged, h, &stage);
        if (!r && (staged.fs->block_bytes != j->block_bytes || staged.fs->blocks != j->blocks ||
                   staged.fs->root != j->root || staged.fs->variant != j->variant)) r = IFS_OFS_CORRUPT;
        if (!r) r = ofs_layout_verify(&staged, (int)j->growth);
        staged.live = live;
        if (!r) r = ofs_layout_hash(&staged, &stage, digest);
        if (!r && !equal(digest, j->stage_hash)) r = IFS_OFS_CORRUPT;
        if (!r && j->phase == 3u) {
            r = ofs_layout_hash(&staged, &target, digest);
            if (!r && !equal(digest, j->stage_hash)) r = IFS_OFS_CORRUPT;
        } else if (!r) {
            r = save(h, journal_path, j, 2u);
            if (!r) r = ofs_layout_commit(&staged, &target);
            if (!r) r = save(h, journal_path, j, 3u);
        }
        if (!r) r = cleanup(h, journal_path, j);
        goto done;
    }
    if (h->open_target(h->context, path, 0, 0, 0, &source)) { r = IFS_OFS_IO_ERROR; goto done; }
    r = ofs_layout_open(&original, h, &source);
    if (r) goto done;
    if (identify) {
        h->print(h->context, 0, "{\"filesystem\":\"affs\",\"variant\":\"OFS\",\"dostype\":%u}\n", original.fs->variant);
        goto done;
    }
    if (analyse) { r = ofs_layout_analyse(&original); goto done; }
    r = copy(j->device, sizeof(j->device), source.path);
    if (!r) r = copy(j->identity, sizeof(j->identity), source.identity);
    if (!r) r = stage_path(h, journal_path, j->stage);
    if (!r && (!safe(j->device) || !safe(j->identity) || !safe(j->stage))) r = IFS_OFS_INVALID;
    if (!r && (h->file_exists(h->context, journal_path) != 0 ||
               h->file_exists(h->context, j->stage) != 0)) r = IFS_OFS_EXISTS;
    if (r) goto done;
    j->bytes = source.bytes; j->block_bytes = original.fs->block_bytes;
    j->blocks = original.fs->blocks; j->root = original.fs->root;
    j->reserved = original.fs->reserved; j->variant = original.fs->variant; j->growth = (uint32_t)growth;
    r = token(h, &source, j, j->token);
    if (!r) r = ofs_layout_source_hash(&original, &source, j->source_hash);
    if (!r) r = ofs_layout_plan(&original, growth);
    if (!r && h->create_stage(h->context, j->stage, j->bytes, &stage)) r = IFS_OFS_IO_ERROR;
    if (!r) stage_created = 1;
    if (!r) r = ofs_layout_write(&original, &stage);
    if (!r) r = ofs_layout_open(&staged, h, &stage);
    if (!r) r = ofs_layout_verify(&staged, growth);
    staged.live = live;
    if (!r) r = ofs_layout_hash(&staged, &stage, j->stage_hash);
    if (!r) { r = save(h, journal_path, j, 1u); if (!r) journal_saved = 1; }
    if (r) goto done;
    h->close_target(h->context, &source);
    if (h->open_target(h->context, path, 1, j->identity, j->bytes, &target)) { r = IFS_OFS_IO_ERROR; goto done; }
    char current[65]; r = token(h, &target, j, current);
    if (!r && !equal(current, j->token)) r = IFS_OFS_CORRUPT;
    if (!r) r = ofs_layout_source_hash(&original, &target, current);
    if (!r && !equal(current, j->source_hash)) r = IFS_OFS_CORRUPT;
    if (!r) r = save(h, journal_path, j, 2u);
    if (!r) r = ofs_layout_commit(&staged, &target);
    if (!r) r = save(h, journal_path, j, 3u);
    if (!r) r = cleanup(h, journal_path, j);
done:
    ofs_layout_close(&staged); ofs_layout_close(&original);
    h->close_target(h->context, &source); h->close_target(h->context, &stage);
    h->close_target(h->context, &target);
    if (stage_created && !journal_saved) (void)h->remove_file(h->context, j->stage);
    h->release(j);
    if (r) h->print(h->context, 1, "OFS module operation failed (%d); retained journals must be recovered before new writes.\n", r);
    if (!identify && !analyse) {
        if (!r && live) h->print(h->context, 0, "@@LIVE_RESET {\"reason\":\"verified OFS.so post-operation map\"}\n");
        result(h, mode, r);
    }
    return r == IFS_DEFRAG_STOPPED ? r : r ? 1 : 0;
}
static const IfsDefragModuleV2 module = {IFS_DEFRAG_ABI_V2, sizeof(module), "ofs",
    IFS_DEFRAG_ANALYSE | IFS_DEFRAG_RELAYOUT | IFS_DEFRAG_GROWTH | IFS_DEFRAG_RECOVER, run};
__attribute__((visibility("default"))) const IfsDefragModuleV2 *
infiltrator_filesystem_defrag_v2(void) { return &module; }
