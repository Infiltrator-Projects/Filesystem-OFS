/* Infiltrator Filesystems — OFS Linux storage adapter.
 * Allocation bitmap, regular-file I/O and inode integration are one host
 * storage responsibility around the canonical OFS engine.
 */

/*
 * Project-authored Linux allocation-bitmap adapter for Amiga OFS. On-disk arithmetic and bit-selection rules are owned by
 * the canonical OFS core; this file binds those rules to Linux buffers,
 * locking and dirty-state handling.
 */

#include "linux_adapter.h"

#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/ktime.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/math64.h>

static void ifs_ofs_bitmap_drop_cache(struct affs_sb_info *sbi)
{
    affs_brelse(sbi->s_bmap_bh);
    sbi->s_bmap_bh = NULL;
    sbi->s_last_bmap = ~0U;
}

static struct buffer_head *ifs_ofs_bitmap_get(
    struct super_block *sb, const u32 bitmap_index)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh;

    if (!sbi->s_bitmap || bitmap_index >= sbi->s_bmap_count)
        return NULL;

    if (sbi->s_last_bmap == bitmap_index && sbi->s_bmap_bh)
        return sbi->s_bmap_bh;

    ifs_ofs_bitmap_drop_cache(sbi);
    bh = affs_bread(sb, sbi->s_bitmap[bitmap_index].bm_key);
    if (!bh)
        return NULL;

    sbi->s_bmap_bh = bh;
    sbi->s_last_bmap = bitmap_index;
    return bh;
}

static u32 ifs_ofs_bitmap_valid_bits(
    const struct affs_sb_info *sbi, const u32 bitmap_index)
{
    const u32 data_blocks =
        (u32)sbi->s_partition_size - (u32)sbi->s_reserved;
    const u32 base = bitmap_index * sbi->s_bmap_bits;
    const u32 remaining = data_blocks - base;

    return remaining < sbi->s_bmap_bits ? remaining : sbi->s_bmap_bits;
}

u32 affs_count_free_blocks(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    u32 total = 0U;
    u32 index;

    /* A read-only filesystem still has free blocks. The cached bitmap
     * accounting is observational and must remain available to statfs. */
    if (!sbi->s_bitmap)
        return 0U;

    mutex_lock(&sbi->s_bmlock);
    for (index = 0U; index < sbi->s_bmap_count; ++index)
        total += sbi->s_bitmap[index].bm_free;
    mutex_unlock(&sbi->s_bmlock);

    return total;
}

void affs_free_block(struct super_block *sb, u32 block)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bm;
    struct buffer_head *bh;
    __be32 *words;
    u32 bitmap_index;
    u32 bit_index;
    u32 word_index;
    u32 mask;
    u32 value;
    u32 checksum;

    if (ifs_ofs_bitmap_location(
            block, (u32)sbi->s_reserved, (u32)sbi->s_partition_size,
            sbi->s_bmap_bits, &bitmap_index, &bit_index) != 0) {
        affs_error(sb, "affs_free_block",
                   "Block %u outside partition", block);
        return;
    }

    mutex_lock(&sbi->s_bmlock);

    if (!sbi->s_bitmap || bitmap_index >= sbi->s_bmap_count) {
        affs_error(sb, "affs_free_block",
                   "Bitmap index %u is invalid", bitmap_index);
        goto out_unlock;
    }

    bm = &sbi->s_bitmap[bitmap_index];
    bh = ifs_ofs_bitmap_get(sb, bitmap_index);
    if (!bh) {
        affs_error(sb, "affs_free_block",
                   "Cannot read bitmap block %u", bm->bm_key);
        goto out_unlock;
    }

    word_index = 1U + bit_index / 32U;
    if (word_index >= sb->s_blocksize / sizeof(__be32)) {
        affs_error(sb, "affs_free_block",
                   "Bitmap bit %u escapes bitmap block", bit_index);
        goto out_unlock;
    }

    words = (__be32 *)bh->b_data;
    mask = ifs_ofs_bitmap_bit_mask(bit_index);
    value = be32_to_cpu(words[word_index]);

    if ((value & mask) != 0U) {
        affs_warning(sb, "affs_free_block",
                     "Block %u is already free", block);
        goto out_unlock;
    }

    words[word_index] = cpu_to_be32(value | mask);
    checksum = be32_to_cpu(words[0]);
    words[0] = cpu_to_be32(checksum - mask);
    bm->bm_free++;

    mark_buffer_dirty(bh);
    affs_mark_sb_dirty(sb);

out_unlock:
    mutex_unlock(&sbi->s_bmlock);
}

static int ifs_ofs_allocate_bitmap_range(
    struct inode *inode,
    const u32 bitmap_index,
    const u32 first_bit,
    const u32 end_bit,
    u32 *const allocated_block)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bm = &sbi->s_bitmap[bitmap_index];
    struct buffer_head *bh;
    __be32 *words;
    u32 first_word;
    u32 last_word;
    u32 word_index;
    u32 bitmap_base;

    if (allocated_block == NULL || first_bit >= end_bit)
        return 0;

    bh = ifs_ofs_bitmap_get(sb, bitmap_index);
    if (!bh)
        return -EIO;

    words = (__be32 *)bh->b_data;
    first_word = first_bit / 32U;
    last_word = (end_bit - 1U) / 32U;
    bitmap_base = bitmap_index * sbi->s_bmap_bits;

    for (word_index = first_word; word_index <= last_word; ++word_index) {
        const u32 word_base = word_index * 32U;
        const u32 start =
            first_bit > word_base ? first_bit - word_base : 0U;
        const u32 range_end =
            end_bit - word_base < 32U ? end_bit - word_base : 32U;
        const u32 disk_word_index = word_index + 1U;
        u32 value;
        u32 first;
        u32 run_mask;
        u32 run_length;
        u32 checksum;
        u32 absolute;

        if (disk_word_index >= sb->s_blocksize / sizeof(__be32))
            return -EUCLEAN;

        value = be32_to_cpu(words[disk_word_index]);
        if (ifs_ofs_bitmap_select_free_run(
                value, start, range_end,
                &first, &run_mask, &run_length) != 0)
            continue;

        absolute = (u32)sbi->s_reserved + bitmap_base +
                   word_base + first;
        if (!affs_validblock(sb, absolute))
            return -EUCLEAN;

        if (bm->bm_free < run_length) {
            run_length = 1U;
            run_mask = ifs_ofs_bitmap_bit_mask(first);
        }

        words[disk_word_index] = cpu_to_be32(value & ~run_mask);
        checksum = be32_to_cpu(words[0]);
        words[0] = cpu_to_be32(checksum + run_mask);

        bm->bm_free -= run_length;
        AFFS_I(inode)->i_lastalloc = absolute;
        AFFS_I(inode)->i_pa_cnt = (int)run_length - 1;

        mark_buffer_dirty(bh);
        affs_mark_sb_dirty(sb);
        *allocated_block = absolute;
        return 1;
    }

    return 0;
}

u32 affs_alloc_block(struct inode *inode, u32 goal)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    u32 start_bitmap;
    u32 start_bit;
    u32 allocated = 0U;
    u32 step;
    int result;

    if (AFFS_I(inode)->i_pa_cnt > 0) {
        const u32 next = AFFS_I(inode)->i_lastalloc + 1U;

        if (affs_validblock(sb, next)) {
            AFFS_I(inode)->i_lastalloc = next;
            AFFS_I(inode)->i_pa_cnt--;
            return next;
        }
        AFFS_I(inode)->i_pa_cnt = 0;
    }

    if (!sbi->s_bitmap || sbi->s_bmap_count == 0U ||
        sbi->s_bmap_bits == 0U)
        return 0U;

    if (!affs_validblock(sb, goal))
        goal = (u32)sbi->s_reserved;

    if (ifs_ofs_bitmap_location(
            goal, (u32)sbi->s_reserved, (u32)sbi->s_partition_size,
            sbi->s_bmap_bits, &start_bitmap, &start_bit) != 0)
        return 0U;

    mutex_lock(&sbi->s_bmlock);

    for (step = 0U; step < sbi->s_bmap_count; ++step) {
        const u32 bitmap_index =
            (start_bitmap + step) % sbi->s_bmap_count;
        const u32 valid_bits =
            ifs_ofs_bitmap_valid_bits(sbi, bitmap_index);
        const u32 begin =
            step == 0U ? start_bit : 0U;

        if (sbi->s_bitmap[bitmap_index].bm_free == 0U ||
            begin >= valid_bits)
            continue;

        result = ifs_ofs_allocate_bitmap_range(
            inode, bitmap_index, begin, valid_bits, &allocated);
        if (result < 0) {
            affs_error(sb, "affs_alloc_block",
                       "Cannot use bitmap block %u",
                       sbi->s_bitmap[bitmap_index].bm_key);
            goto out;
        }
        if (result > 0)
            goto out;
    }

    if (start_bit > 0U && sbi->s_bitmap[start_bitmap].bm_free != 0U) {
        result = ifs_ofs_allocate_bitmap_range(
            inode, start_bitmap, 0U, start_bit, &allocated);
        if (result < 0) {
            affs_error(sb, "affs_alloc_block",
                       "Cannot wrap within bitmap block %u",
                       sbi->s_bitmap[start_bitmap].bm_key);
            allocated = 0U;
        }
    }

out:
    mutex_unlock(&sbi->s_bmlock);
    return allocated;
}

static void ifs_ofs_bitmap_discard(struct affs_sb_info *sbi)
{
    ifs_ofs_bitmap_drop_cache(sbi);
    kfree(sbi->s_bitmap);
    sbi->s_bitmap = NULL;
    sbi->s_bmap_count = 0U;
    sbi->s_bmap_bits = 0U;
}

static int ifs_ofs_bitmap_normalize_tail(
    struct super_block *sb,
    struct buffer_head *bh,
    struct affs_bm_info *last_bitmap,
    const u32 valid_bits,
    const bool readonly)
{
    __be32 *words = (__be32 *)bh->b_data;
    const u32 data_word_count =
        (u32)(sb->s_blocksize / sizeof(__be32)) - 1U;
    const u32 full_words = valid_bits / 32U;
    const u32 partial_bits = valid_bits % 32U;
    u32 index;
    u32 valid_free = 0U;
    bool changed = false;

    for (index = 0U; index < data_word_count; ++index) {
        u32 valid_mask;
        const u32 old_value = be32_to_cpu(words[index + 1U]);
        u32 new_value;

        if (index < full_words)
            valid_mask = 0xffffffffU;
        else if (index == full_words && partial_bits != 0U)
            valid_mask = ifs_ofs_bitmap_valid_word_mask(partial_bits);
        else
            valid_mask = 0U;

        new_value = old_value & valid_mask;
        valid_free += hweight32(new_value);

        /* Read-only mount must never rewrite invalid tail bits, modify the
         * bitmap checksum, or mark a buffer dirty just to count space. */
        if (!readonly && new_value != old_value) {
            words[index + 1U] = cpu_to_be32(new_value);
            changed = true;
        }
    }

    if (changed) {
        words[0] = 0;
        words[0] = cpu_to_be32(0U - affs_checksum_block(sb, bh));
        mark_buffer_dirty(bh);
    }

    last_bitmap->bm_free = valid_free;
    return 0;
}

/*
 * Read a bounded contiguous extent of 512-byte OFS bitmap blocks with one
 * block-layer BIO instead of issuing 61k separate half-kilobyte reads.
 * It is mount-time, read-only I/O: allocated pages are temporary, and the
 * existing buffer-head path is retained for fragmented or unsupported media.
 *
 * This does not defer metadata validation. Every bitmap still passes the
 * canonical AmigaDOS checksum and free-bit accounting before mount completes.
 */
static int ifs_ofs_account_bitmap_extent(
    struct super_block *sb, struct affs_bm_info *bitmap,
    const u32 first, const u32 count,
    u64 *const io_ns, u64 *const checksum_ns, u64 *const counts_ns)
{
    enum { IFS_OFS_BIO_PAGES = 16, IFS_OFS_BIO_BYTES = 16 * PAGE_SIZE };
    struct page *pages[IFS_OFS_BIO_PAGES] = { NULL };
    struct bio *bio = NULL;
    unsigned int page_count;
    unsigned int page_index;
    u32 index;
    int status = 0;
    u64 step_ns;

    if (sb->s_blocksize != 512U || count == 0U ||
        count > IFS_OFS_BIO_BYTES / 512U)
        return -EOPNOTSUPP;

    page_count = DIV_ROUND_UP(count * 512U, (u32)PAGE_SIZE);
    for (page_index = 0U; page_index < page_count; ++page_index) {
        pages[page_index] = alloc_page(GFP_KERNEL);
        if (!pages[page_index]) {
            status = -ENOMEM;
            goto out;
        }
    }

    bio = bio_alloc(sb->s_bdev, page_count, REQ_OP_READ, GFP_KERNEL);
    if (!bio) {
        status = -ENOMEM;
        goto out;
    }

    /* On this fast path one OFS 512B block equals one 512B BIO sector.
     * All block numbers were already validated when collecting bitmap keys. */
    bio->bi_iter.bi_sector = (sector_t)bitmap[first].bm_key;
    for (page_index = 0U; page_index < page_count; ++page_index) {
        const unsigned int offset_bytes = page_index * PAGE_SIZE;
        const unsigned int remaining_bytes = count * 512U - offset_bytes;
        const unsigned int bytes = min_t(
            unsigned int, remaining_bytes, PAGE_SIZE);

        if (bio_add_page(bio, pages[page_index], bytes, 0U) != bytes) {
            /* Some block queues cannot accept the proposed extent. Retain
             * the portable synchronous buffer-head fallback instead. */
            status = -EOPNOTSUPP;
            goto out;
        }
    }

    step_ns = ktime_get_ns();
    status = submit_bio_wait(bio);
    *io_ns += ktime_get_ns() - step_ns;
    if (status)
        goto out;

    for (index = 0U; index < count; ++index) {
        const u32 byte_offset = index * 512U;
        const unsigned int p = byte_offset / PAGE_SIZE;
        const unsigned int off = byte_offset % PAGE_SIZE;
        const u8 *const data = (const u8 *)page_address(pages[p]) + off;

        step_ns = ktime_get_ns();
        if (ifs_ofs_block_checksum(data, 512U) != 0U) {
            status = -EUCLEAN;
            goto out;
        }
        *checksum_ns += ktime_get_ns() - step_ns;

        step_ns = ktime_get_ns();
        bitmap[first + index].bm_free =
            memweight(data + sizeof(__be32), 512U - sizeof(__be32));
        *counts_ns += ktime_get_ns() - step_ns;
    }

out:
    if (bio)
        bio_put(bio);
    for (page_index = 0U; page_index < ARRAY_SIZE(pages); ++page_index)
        if (pages[page_index])
            __free_page(pages[page_index]);
    return status;
}

int affs_init_bitmap(struct super_block *sb, int *flags)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bitmap;
    struct buffer_head *bitmap_bh = NULL;
    struct buffer_head *extension_bh = NULL;
    __be32 *key_words;
    u32 key_index;
    u32 key_end;
    u32 words_per_block;
    u32 index;
    u32 data_blocks;
    u32 last_valid_bits;
    u64 began_ns;
    u64 keys_ns;
    u64 batched_io_ns = 0U;
    u64 reads_ns = 0U;
    u64 checksums_ns = 0U;
    u64 counts_ns = 0U;
    u64 measured_ns;
    u32 batches = 0U;
    u32 batched_blocks = 0U;
    u32 fallback_blocks = 0U;
    int result = 0;

    /* A real 120 GiB OFS partition holds ~61k 512B bitmap blocks.
     * A bounded direct BIO fast path coalesces adjacent blocks, while the
     * portable bh reader handles fragmented/unsupported bitmap layouts. */
    enum { IFS_OFS_BITMAP_BIO_BLOCKS = 128 };

    began_ns = ktime_get_ns();

    /* Read-only mounts still need validated bitmap metadata for statfs.
     * Loading and counting it is safe; normalisation is write-gated below. */
    if (be32_to_cpu(AFFS_ROOT_TAIL(sb, sbi->s_root_bh)->bm_flag) == 0U) {
        pr_notice("Bitmap invalid - mounting %s read only\n", sb->s_id);
        *flags |= SB_RDONLY;
        return 0;
    }

    /* Reconfiguration from ro to rw must replace the previous cached bitmap
     * rather than losing the allocation pointer and its accounting. */
    ifs_ofs_bitmap_discard(sbi);

    if (ifs_ofs_bitmap_geometry(
            (u32)sb->s_blocksize, (u32)sbi->s_reserved,
            (u32)sbi->s_partition_size,
            &sbi->s_bmap_bits, &sbi->s_bmap_count) != 0) {
        pr_err("Invalid AmigaDOS bitmap geometry\n");
        return -EINVAL;
    }

    bitmap = kcalloc(sbi->s_bmap_count, sizeof(*bitmap), GFP_KERNEL);
    if (!bitmap)
        return -ENOMEM;
    sbi->s_bitmap = bitmap;

    words_per_block = (u32)(sb->s_blocksize / sizeof(__be32));
    if (words_per_block < 50U) {
        result = -EUCLEAN;
        goto fail;
    }

    key_words = (__be32 *)sbi->s_root_bh->b_data;
    key_index = words_per_block - 49U;
    key_end = key_index + AFFS_ROOT_BMAPS;

    /* Phase 1: resolve the full bitmap key chain before issuing read-ahead.
     * This uses the existing extension format and validates partition bounds,
     * so malformed metadata cannot queue I/O to arbitrary device sectors.
     * The resulting keys remain authoritative for the normal allocation path. */
    for (index = 0U; index < sbi->s_bmap_count; ++index) {
        u32 key;

        if (key_index >= key_end) {
            const u32 extension_key = be32_to_cpu(key_words[key_index]);

            if (!ifs_ofs_data_block_valid(
                    extension_key, (u32)sbi->s_reserved,
                    (u32)sbi->s_partition_size)) {
                result = -EUCLEAN;
                goto fail;
            }

            affs_brelse(extension_bh);
            extension_bh = affs_bread(sb, extension_key);
            if (!extension_bh) {
                result = -EIO;
                goto fail;
            }

            key_words = (__be32 *)extension_bh->b_data;
            key_index = 0U;
            key_end = words_per_block - 1U;
        }

        key = be32_to_cpu(key_words[key_index++]);
        if (!ifs_ofs_data_block_valid(
                key, (u32)sbi->s_reserved,
                (u32)sbi->s_partition_size)) {
            result = -EUCLEAN;
            goto fail;
        }
        bitmap[index].bm_key = key;
    }

    affs_brelse(extension_bh);
    extension_bh = NULL;
    keys_ns = ktime_get_ns() - began_ns;

    /* Phase 2: coalesce physically adjacent bitmap keys. The canonical
     * key chain is not assumed contiguous: fragmented runs fall back to
     * the existing buffer-head reader, one validated block at a time. */
    for (index = 0U; index < sbi->s_bmap_count;) {
        const u32 key = bitmap[index].bm_key;
        u32 run = 1U;
        u64 step_ns;

        if (sb->s_blocksize == 512U) {
            while (run < IFS_OFS_BITMAP_BIO_BLOCKS &&
                   index + run < sbi->s_bmap_count &&
                   bitmap[index + run].bm_key == key + run)
                ++run;
        }

        if (run >= 8U) {
            result = ifs_ofs_account_bitmap_extent(
                sb, bitmap, index, run,
                &batched_io_ns, &checksums_ns, &counts_ns);
            if (result == 0) {
                ++batches;
                batched_blocks += run;
                index += run;
                continue;
            }
            if (result == -EUCLEAN) {
                pr_warn("Bitmap checksum invalid in extent from %u on %s"
                        " - mounting read only\n", key, sb->s_id);
                *flags |= SB_RDONLY;
                result = 0;
                goto readonly;
            }
            if (result != -ENOMEM && result != -EOPNOTSUPP)
                goto fail;
            /* Resource/queue limitation: validate through bh below.
             * Re-attempting contiguous BIO allocation on the next key is
             * harmless, but avoid per-key allocations for this run. */
        }

        affs_brelse(bitmap_bh);
        bitmap_bh = NULL;

        step_ns = ktime_get_ns();
        bitmap_bh = affs_bread(sb, key);
        reads_ns += ktime_get_ns() - step_ns;
        if (!bitmap_bh) {
            result = -EIO;
            goto fail;
        }

        step_ns = ktime_get_ns();
        if (affs_checksum_block(sb, bitmap_bh) != 0U) {
            pr_warn("Bitmap %u invalid - mounting %s read only\n",
                    key, sb->s_id);
            *flags |= SB_RDONLY;
            result = 0;
            goto readonly;
        }
        checksums_ns += ktime_get_ns() - step_ns;

        step_ns = ktime_get_ns();
        bitmap[index].bm_free =
            memweight(bitmap_bh->b_data + sizeof(__be32),
                      sb->s_blocksize - sizeof(__be32));
        counts_ns += ktime_get_ns() - step_ns;
        ++fallback_blocks;
        ++index;
    }

    /* Tail normalization uses a writable buffer_head when RW, and observes
     * only valid bits when RO. The BIO path never leaves one cached. */
    affs_brelse(bitmap_bh);
    bitmap_bh = affs_bread(sb, bitmap[sbi->s_bmap_count - 1U].bm_key);
    if (!bitmap_bh) {
        result = -EIO;
        goto fail;
    }
    if (affs_checksum_block(sb, bitmap_bh) != 0U) {
        *flags |= SB_RDONLY;
        result = 0;
        goto readonly;
    }

    data_blocks = (u32)sbi->s_partition_size - (u32)sbi->s_reserved;
    last_valid_bits =
        data_blocks - (sbi->s_bmap_count - 1U) * sbi->s_bmap_bits;

    if (last_valid_bits == 0U || last_valid_bits > sbi->s_bmap_bits) {
        result = -EUCLEAN;
        goto fail;
    }

    result = ifs_ofs_bitmap_normalize_tail(
        sb, bitmap_bh, &bitmap[sbi->s_bmap_count - 1U],
        last_valid_bits, (*flags & SB_RDONLY) != 0);

    /* One line per large mount gives actionable kernel-level timings without
     * flooding logs for small filesystem images or individual bitmap blocks.
     * Counters include only their named phase; total covers all phases. */
    if (sbi->s_bmap_count >= 4096U) {
        measured_ns = ktime_get_ns() - began_ns;
        pr_info("OFS bitmap mount: blocks=%u batches=%u batched_blocks=%u "
                "fallback_blocks=%u keys=%llu ms batched_io=%llu ms "
                "reads=%llu ms checksum=%llu ms count=%llu ms total=%llu ms\n",
                sbi->s_bmap_count, batches, batched_blocks, fallback_blocks,
                (unsigned long long)div_u64(keys_ns, 1000000U),
                (unsigned long long)div_u64(batched_io_ns, 1000000U),
                (unsigned long long)div_u64(reads_ns, 1000000U),
                (unsigned long long)div_u64(checksums_ns, 1000000U),
                (unsigned long long)div_u64(counts_ns, 1000000U),
                (unsigned long long)div_u64(measured_ns, 1000000U));
    }
    goto out;

readonly:
    ifs_ofs_bitmap_discard(sbi);
    goto out;

fail:
    ifs_ofs_bitmap_discard(sbi);

out:
    affs_brelse(bitmap_bh);
    affs_brelse(extension_bh);
    return result;
}

void affs_free_bitmap(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    ifs_ofs_bitmap_discard(sbi);
}


/* ===== regular-file I/O ===== */
/*
 * Project-authored Linux regular-file adapter for Amiga OFS.
 *
 * Filesystem geometry is provided by the canonical AmigaDOS core. This unit
 * owns Linux page-cache, buffer-head and VFS integration only.
 */

#include "linux_adapter.h"

#include <linux/uio.h>
#include <linux/blkdev.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>

static int ifs_ofs_map_block(
    struct inode *inode, sector_t logical,
    struct buffer_head *result, int create);

static int ifs_ofs_file_open(struct inode *inode, struct file *file)
{
    atomic_inc(&AFFS_I(inode)->i_opencnt);
    return 0;
}

static int ifs_ofs_file_release(struct inode *inode, struct file *file)
{
    int result = 0;

    if (atomic_dec_and_test(&AFFS_I(inode)->i_opencnt)) {
        inode_lock(inode);
        if (inode->i_size != AFFS_I(inode)->mmu_private)
            result = affs_truncate(inode);
        affs_free_prealloc(inode);
        inode_unlock(inode);
    }
    return result;
}

static bool ifs_ofs_extension_valid(
    struct inode *inode, struct buffer_head *bh, u32 index)
{
    struct super_block *sb = inode->i_sb;
    const u32 expected_type = index == 0U ? T_SHORT : T_LIST;
    const struct affs_head *head;
    const struct affs_tail *tail;

    if (!bh || bh->b_blocknr > U32_MAX ||
        !affs_validblock(sb, (u32)bh->b_blocknr))
        return false;

    head = AFFS_HEAD(bh);
    tail = AFFS_TAIL(sb, bh);

    if (affs_checksum_block(sb, bh) != 0U ||
        be32_to_cpu(head->ptype) != expected_type ||
        be32_to_cpu(head->key) != (u32)bh->b_blocknr ||
        be32_to_cpu(tail->stype) != ST_FILE ||
        be32_to_cpu(head->block_count) >
            (u32)AFFS_SB(sb)->s_hashsize)
        return false;

    if (be32_to_cpu(head->block_count) == 0U) {
        if (head->first_data != 0)
            return false;
    } else if (head->first_data != AFFS_BLOCK(sb, bh, 0U)) {
        return false;
    }

    if (index != 0U &&
        be32_to_cpu(tail->parent) != (u32)inode->i_ino)
        return false;

    return true;
}

static void ifs_ofs_cache_extension(
    struct inode *inode, struct buffer_head *bh, u32 index)
{
    struct affs_inode_info *info = AFFS_I(inode);

    if (info->i_ext_bh == bh && info->i_ext_last == index)
        return;

    affs_brelse(info->i_ext_bh);
    info->i_ext_bh = bh;
    info->i_ext_last = index;
    get_bh(bh);
}

static void ifs_ofs_clear_extension_cache(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    affs_brelse(info->i_ext_bh);
    info->i_ext_bh = NULL;
    info->i_ext_last = ~1U;
    info->i_lc_size = 0U;

    if (info->i_lc)
        memset(info->i_lc, 0, AFFS_LC_SIZE * sizeof(*info->i_lc));
    if (info->i_ac)
        memset(info->i_ac, 0, AFFS_AC_SIZE * sizeof(*info->i_ac));
}

static struct buffer_head *ifs_ofs_read_extension(
    struct inode *inode, u32 wanted)
{
    struct affs_inode_info *info = AFFS_I(inode);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    u32 index;

    if (wanted >= info->i_extcnt)
        return ERR_PTR(-EUCLEAN);

    if (info->i_ext_bh && info->i_ext_last == wanted) {
        get_bh(info->i_ext_bh);
        return info->i_ext_bh;
    }

    if (info->i_ext_bh && info->i_ext_last < wanted) {
        bh = info->i_ext_bh;
        index = info->i_ext_last;
        get_bh(bh);
    } else {
        bh = affs_bread(sb, (u32)inode->i_ino);
        index = 0U;
    }

    if (!bh)
        return ERR_PTR(-EIO);

    if (!ifs_ofs_extension_valid(inode, bh, index)) {
        affs_brelse(bh);
        return ERR_PTR(-EUCLEAN);
    }

    while (index < wanted) {
        const u32 next =
            be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
        struct buffer_head *next_bh;

        if (next == 0U || !affs_validblock(sb, next)) {
            affs_brelse(bh);
            return ERR_PTR(-EUCLEAN);
        }

        next_bh = affs_bread(sb, next);
        if (!next_bh) {
            affs_brelse(bh);
            return ERR_PTR(-EIO);
        }

        affs_brelse(bh);
        bh = next_bh;
        index++;

        if (!ifs_ofs_extension_valid(inode, bh, index)) {
            affs_brelse(bh);
            return ERR_PTR(-EUCLEAN);
        }
    }

    ifs_ofs_cache_extension(inode, bh, wanted);
    return bh;
}

static int ifs_ofs_zero_new_physical_block(
    struct super_block *sb, u32 block)
{
    struct buffer_head *bh = affs_getzeroblk(sb, block);

    if (!bh)
        return -EIO;

    mark_buffer_dirty(bh);
    affs_brelse(bh);
    return 0;
}

static int ifs_ofs_append_extension_and_data(
    struct inode *inode,
    u32 extension_index,
    u32 data_block,
    struct buffer_head **extension_out)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    struct buffer_head *previous;
    struct buffer_head *new_extension;
    u32 extension_block;

    if (extension_index == 0U ||
        extension_index != info->i_extcnt)
        return -EUCLEAN;

    previous = ifs_ofs_read_extension(inode, extension_index - 1U);
    if (IS_ERR(previous))
        return PTR_ERR(previous);

    if (AFFS_TAIL(sb, previous)->extension != 0) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    extension_block = affs_alloc_block(inode, (u32)previous->b_blocknr);
    if (extension_block == 0U) {
        affs_brelse(previous);
        return -ENOSPC;
    }

    new_extension = affs_getzeroblk(sb, extension_block);
    if (!new_extension) {
        affs_free_block(sb, extension_block);
        affs_brelse(previous);
        return -EIO;
    }

    AFFS_HEAD(new_extension)->ptype = cpu_to_be32(T_LIST);
    AFFS_HEAD(new_extension)->key = cpu_to_be32(extension_block);
    AFFS_HEAD(new_extension)->block_count = cpu_to_be32(1U);
    AFFS_HEAD(new_extension)->first_data = cpu_to_be32(data_block);
    AFFS_BLOCK(sb, new_extension, 0U) = cpu_to_be32(data_block);
    AFFS_TAIL(sb, new_extension)->parent =
        cpu_to_be32((u32)inode->i_ino);
    AFFS_TAIL(sb, new_extension)->stype = cpu_to_be32(ST_FILE);
    affs_fix_checksum(sb, new_extension);
    mark_buffer_dirty_inode(new_extension, inode);

    AFFS_TAIL(sb, previous)->extension = cpu_to_be32(extension_block);
    affs_fix_checksum(sb, previous);
    mark_buffer_dirty_inode(previous, inode);
    affs_brelse(previous);

    info->i_extcnt++;
    ifs_ofs_cache_extension(inode, new_extension, extension_index);
    *extension_out = new_extension;
    return 0;
}

static int ifs_ofs_map_existing_block(
    struct inode *inode, u32 logical,
    struct buffer_head *result)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *extension;
    u32 extension_index;
    u32 entry_index;
    u32 physical;
    int status;

    status = ifs_ofs_file_block_location(
        logical, (u32)AFFS_SB(sb)->s_hashsize,
        &extension_index, &entry_index);
    if (status != 0)
        return -EUCLEAN;

    extension = ifs_ofs_read_extension(inode, extension_index);
    if (IS_ERR(extension))
        return PTR_ERR(extension);

    physical = be32_to_cpu(
        AFFS_BLOCK(sb, extension, entry_index));
    affs_brelse(extension);

    if (physical == 0U ||
        !affs_validblock(sb, physical))
        return -EUCLEAN;

    map_bh(result, sb, physical);
    return 0;
}

static int ifs_ofs_append_block(
    struct inode *inode, u32 logical,
    struct buffer_head *result)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    struct buffer_head *extension = NULL;
    u32 extension_index;
    u32 entry_index;
    u32 physical;
    int status;

    status = ifs_ofs_file_block_location(
        logical, (u32)AFFS_SB(sb)->s_hashsize,
        &extension_index, &entry_index);
    if (status != 0)
        return -EUCLEAN;

    if (logical != info->i_blkcnt ||
        extension_index > info->i_extcnt)
        return -EUCLEAN;

    if (extension_index < info->i_extcnt) {
        extension = ifs_ofs_read_extension(inode, extension_index);
        if (IS_ERR(extension))
            return PTR_ERR(extension);

        if (AFFS_BLOCK(sb, extension, entry_index) != 0) {
            affs_brelse(extension);
            return -EUCLEAN;
        }

        physical = affs_alloc_block(inode, (u32)extension->b_blocknr);
        if (physical == 0U) {
            affs_brelse(extension);
            return -ENOSPC;
        }

        status = ifs_ofs_zero_new_physical_block(sb, physical);
        if (status != 0) {
            affs_free_block(sb, physical);
            affs_brelse(extension);
            return status;
        }

        AFFS_BLOCK(sb, extension, entry_index) = cpu_to_be32(physical);
        AFFS_HEAD(extension)->block_count =
            cpu_to_be32(entry_index + 1U);
        if (entry_index == 0U)
            AFFS_HEAD(extension)->first_data =
                cpu_to_be32(physical);
        affs_fix_checksum(sb, extension);
        mark_buffer_dirty_inode(extension, inode);
    } else {
        struct buffer_head *previous;

        if (entry_index != 0U || extension_index == 0U)
            return -EUCLEAN;

        previous = ifs_ofs_read_extension(
            inode, extension_index - 1U);
        if (IS_ERR(previous))
            return PTR_ERR(previous);

        physical = affs_alloc_block(
            inode, (u32)previous->b_blocknr);
        affs_brelse(previous);
        if (physical == 0U)
            return -ENOSPC;

        status = ifs_ofs_zero_new_physical_block(sb, physical);
        if (status != 0) {
            affs_free_block(sb, physical);
            return status;
        }

        status = ifs_ofs_append_extension_and_data(
            inode, extension_index, physical, &extension);
        if (status != 0) {
            affs_free_block(sb, physical);
            return status;
        }
    }

    info->i_blkcnt++;
    if (info->mmu_private <= U32_MAX - AFFS_SB(sb)->s_data_blksize)
        info->mmu_private += AFFS_SB(sb)->s_data_blksize;

    map_bh(result, sb, physical);
    set_buffer_new(result);
    affs_brelse(extension);
    mark_inode_dirty(inode);
    return 0;
}

static int ifs_ofs_map_block(
    struct inode *inode, sector_t logical,
    struct buffer_head *result, int create)
{
    struct affs_inode_info *info = AFFS_I(inode);
    int status;

    if (logical > U32_MAX)
        return -EFBIG;

    if (AFFS_SB(inode->i_sb)->s_hashsize <= 0)
        return -EUCLEAN;

    affs_lock_ext(inode);

    if ((u32)logical < info->i_blkcnt) {
        status = ifs_ofs_map_existing_block(
            inode, (u32)logical, result);
    } else if ((u32)logical == info->i_blkcnt && create) {
        status = ifs_ofs_append_block(
            inode, (u32)logical, result);
    } else if ((u32)logical == info->i_blkcnt && !create) {
        status = 0;
    } else {
        status = -EFBIG;
    }

    affs_unlock_ext(inode);
    return status;
}

static int ifs_ofs_writepages(
    struct address_space *mapping, struct writeback_control *control)
{
    return mpage_writepages(mapping, control, ifs_ofs_map_block);
}

static int ifs_ofs_read_folio(
    struct file *file, struct folio *folio)
{
    return block_read_full_folio(folio, ifs_ofs_map_block);
}

static void ifs_ofs_write_failed(
    struct address_space *mapping, loff_t attempted_end)
{
    struct inode *inode = mapping->host;

    if (attempted_end > inode->i_size) {
        truncate_pagecache(inode, inode->i_size);
        affs_truncate(inode);
    }
}

static ssize_t ifs_ofs_direct_io(
    struct kiocb *iocb, struct iov_iter *iter)
{
    struct file *file = iocb->ki_filp;
    struct address_space *mapping = file->f_mapping;
    struct inode *inode = mapping->host;
    const size_t count = iov_iter_count(iter);
    const loff_t offset = iocb->ki_pos;
    ssize_t result;

    if (iov_iter_rw(iter) == WRITE) {
        if (offset < 0 || offset > U32_MAX ||
            count > (size_t)(U32_MAX - (u64)offset))
            return -EFBIG;

        if (AFFS_I(inode)->mmu_private < offset + count)
            return 0;
    }

    result = blockdev_direct_IO(
        iocb, inode, iter, ifs_ofs_map_block);
    if (result < 0 && iov_iter_rw(iter) == WRITE)
        ifs_ofs_write_failed(mapping, offset + count);
    return result;
}

static int ifs_ofs_write_begin(
    IFS_OFS_AOPS_WRITE_CONTEXT, struct address_space *mapping,
    loff_t position, unsigned int length,
    struct folio **folio, void **fsdata)
{
    int result;

    if (position < 0 || position > U32_MAX ||
        length > U32_MAX - (u64)position)
        return -EFBIG;

    result = cont_write_begin(
        IFS_OFS_AOPS_WRITE_CONTEXT_ARG,
        mapping, position, length, folio, fsdata,
        ifs_ofs_map_block,
        &AFFS_I(mapping->host)->mmu_private);
    if (result != 0)
        ifs_ofs_write_failed(mapping, position + length);
    return result;
}

static int ifs_ofs_write_end(
    IFS_OFS_AOPS_WRITE_CONTEXT, struct address_space *mapping,
    loff_t position, unsigned int length, unsigned int copied,
    struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    const int result = generic_write_end(
        IFS_OFS_AOPS_WRITE_CONTEXT_ARG,
        mapping, position, length, copied, folio, fsdata);

    if (result > 0) {
        AFFS_I(inode)->mmu_private = inode->i_size;
        if ((AFFS_I(inode)->i_protect & FIBF_ARCHIVED) != 0U) {
            AFFS_I(inode)->i_protect &= ~FIBF_ARCHIVED;
            mark_inode_dirty(inode);
        }
    }
    return result;
}

static sector_t ifs_ofs_bmap(
    struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(
        mapping, block, ifs_ofs_map_block);
}

const struct address_space_operations affs_aops = {
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .read_folio = ifs_ofs_read_folio,
    .writepages = ifs_ofs_writepages,
    .write_begin = ifs_ofs_write_begin,
    .write_end = ifs_ofs_write_end,
    .direct_IO = ifs_ofs_direct_io,
    .migrate_folio = buffer_migrate_folio,
    .bmap = ifs_ofs_bmap,
};

static int ifs_ofs_map_to_buffer(
    struct inode *inode, u32 logical, bool create,
    bool zero, struct buffer_head **buffer, bool *new_block)
{
    struct buffer_head mapping = { 0 };
    struct buffer_head *bh;
    int result;

    result = ifs_ofs_map_block(
        inode, logical, &mapping, create ? 1 : 0);
    if (result != 0)
        return result;
    if (!buffer_mapped(&mapping))
        return -EUCLEAN;

    if (new_block)
        *new_block = buffer_new(&mapping);

    bh = zero && buffer_new(&mapping) ?
         affs_getzeroblk(inode->i_sb, (u32)mapping.b_blocknr) :
         affs_bread(inode->i_sb, (u32)mapping.b_blocknr);
    if (!bh)
        return -EIO;

    *buffer = bh;
    return 0;
}

static int ifs_ofs_ofs_link_data_block(
    struct inode *inode, u32 logical, u32 physical)
{
    struct buffer_head *previous;
    u32 old_next;
    int result;

    if (logical == 0U)
        return 0;

    result = ifs_ofs_map_to_buffer(
        inode, logical - 1U, false, false,
        &previous, NULL);
    if (result != 0)
        return result;

    if (affs_checksum_block(inode->i_sb, previous) != 0U ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->ptype) != T_DATA ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->key) !=
            (u32)inode->i_ino ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->sequence) != logical) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    old_next = be32_to_cpu(AFFS_DATA_HEAD(previous)->next);
    if (old_next != 0U && old_next != physical) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    if (old_next == 0U) {
        AFFS_DATA_HEAD(previous)->next = cpu_to_be32(physical);
        affs_fix_checksum(inode->i_sb, previous);
        mark_buffer_dirty_inode(previous, inode);
    }

    affs_brelse(previous);
    return 0;
}

static int ifs_ofs_ofs_prepare_block(
    struct inode *inode, u32 logical,
    struct buffer_head **buffer)
{
    struct buffer_head *bh;
    bool new_block = false;
    int result;

    result = ifs_ofs_map_to_buffer(
        inode, logical, true, true, &bh, &new_block);
    if (result != 0)
        return result;

    if (new_block) {
        AFFS_DATA_HEAD(bh)->ptype = cpu_to_be32(T_DATA);
        AFFS_DATA_HEAD(bh)->key = cpu_to_be32((u32)inode->i_ino);
        AFFS_DATA_HEAD(bh)->sequence = cpu_to_be32(logical + 1U);
        AFFS_DATA_HEAD(bh)->size = 0;
        AFFS_DATA_HEAD(bh)->next = 0;
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);

        result = ifs_ofs_ofs_link_data_block(
            inode, logical, (u32)bh->b_blocknr);
        if (result != 0) {
            affs_brelse(bh);
            return result;
        }
    } else if (affs_checksum_block(inode->i_sb, bh) != 0U ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->ptype) != T_DATA ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->key) !=
                   (u32)inode->i_ino ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->sequence) !=
                   logical + 1U) {
        affs_brelse(bh);
        return -EUCLEAN;
    }

    *buffer = bh;
    return 0;
}

static int ifs_ofs_ofs_read_range(
    struct inode *inode, struct folio *folio,
    size_t destination_offset, u64 file_offset, size_t length)
{
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    size_t copied = 0U;

    if (payload == 0U)
        return -EUCLEAN;

    while (copied < length) {
        const u64 file_position = file_offset + copied;
        const u32 logical = (u32)(file_position / payload);
        const u32 within = (u32)(file_position % payload);
        const size_t chunk =
            min_t(size_t, payload - within, length - copied);
        struct buffer_head *bh;
        u32 stored_size;
        int result;

        result = ifs_ofs_map_to_buffer(
            inode, logical, false, false, &bh, NULL);
        if (result != 0)
            return result;

        if (affs_checksum_block(inode->i_sb, bh) != 0U ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->ptype) != T_DATA ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->key) !=
                (u32)inode->i_ino ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->sequence) != logical + 1U) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload ||
            within > stored_size ||
            chunk > stored_size - within) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        memcpy_to_folio(
            folio, destination_offset + copied,
            AFFS_DATA(bh) + within, chunk);
        affs_brelse(bh);
        copied += chunk;
    }

    return 0;
}

static int ifs_ofs_zero_extend_ffs(
    struct inode *inode, u32 target)
{
    const u32 block_size = AFFS_SB(inode->i_sb)->s_data_blksize;
    u64 position = AFFS_I(inode)->mmu_private;

    if (block_size == 0U)
        return -EUCLEAN;

    while (position < target) {
        const u32 logical = (u32)(position / block_size);
        const u32 within = (u32)(position % block_size);
        const u32 chunk = min_t(
            u32, block_size - within, target - (u32)position);
        struct buffer_head *bh;
        int result;

        result = ifs_ofs_map_to_buffer(
            inode, logical, true, false, &bh, NULL);
        if (result != 0)
            return result;

        memset(bh->b_data + within, 0, chunk);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        position += chunk;
    }

    AFFS_I(inode)->mmu_private = target;
    return 0;
}

static int ifs_ofs_zero_extend_ofs(
    struct inode *inode, u32 target)
{
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    u64 position = AFFS_I(inode)->mmu_private;

    if (payload == 0U)
        return -EUCLEAN;

    while (position < target) {
        const u32 logical = (u32)(position / payload);
        const u32 within = (u32)(position % payload);
        const u32 chunk = min_t(
            u32, payload - within, target - (u32)position);
        struct buffer_head *bh;
        u32 stored_size;
        int result;

        result = ifs_ofs_ofs_prepare_block(inode, logical, &bh);
        if (result != 0)
            return result;

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload || stored_size < within) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        memset(AFFS_DATA(bh) + within, 0, chunk);
        if (stored_size < within + chunk)
            AFFS_DATA_HEAD(bh)->size =
                cpu_to_be32(within + chunk);
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        position += chunk;
    }

    AFFS_I(inode)->mmu_private = target;
    return 0;
}

static int ifs_ofs_ofs_read_folio(
    struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    const u64 start = folio_pos(folio);
    size_t length = 0U;
    int result = 0;

    if (start < (u64)inode->i_size)
        length = min_t(
            u64, folio_size(folio), (u64)inode->i_size - start);

    if (length != 0U)
        result = ifs_ofs_ofs_read_range(
            inode, folio, 0U, start, length);

    if (result == 0) {
        if (length < folio_size(folio))
            folio_zero_segment(folio, length, folio_size(folio));
        folio_mark_uptodate(folio);
    }

    folio_unlock(folio);
    return result;
}

static int ifs_ofs_ofs_write_begin(
    IFS_OFS_AOPS_WRITE_CONTEXT, struct address_space *mapping,
    loff_t position, unsigned int length,
    struct folio **folio_out, void **fsdata)
{
    struct inode *inode = mapping->host;
    struct folio *folio;
    const u64 end = (u64)position + length;

    (void)IFS_OFS_AOPS_WRITE_CONTEXT_ARG;
    const pgoff_t index = position >> PAGE_SHIFT;
    const u64 folio_start = (u64)index << PAGE_SHIFT;
    size_t readable = 0U;
    int result;

    if (position < 0 || end > U32_MAX)
        return -EFBIG;

    if ((u64)position > (u64)AFFS_I(inode)->mmu_private) {
        result = ifs_ofs_zero_extend_ofs(
            inode, (u32)position);
        if (result != 0)
            return result;
    }

    folio = __filemap_get_folio(
        mapping, index, FGP_WRITEBEGIN,
        mapping_gfp_mask(mapping));
    if (IS_ERR(folio))
        return PTR_ERR(folio);

    *folio_out = folio;

    if (folio_test_uptodate(folio))
        return 0;

    if (folio_start < (u64)inode->i_size)
        readable = min_t(
            u64, folio_size(folio),
            (u64)inode->i_size - folio_start);

    if (readable != 0U) {
        result = ifs_ofs_ofs_read_range(
            inode, folio, 0U, folio_start, readable);
        if (result != 0) {
            folio_unlock(folio);
            folio_put(folio);
            return result;
        }
    }

    if (readable < folio_size(folio))
        folio_zero_segment(folio, readable, folio_size(folio));
    folio_mark_uptodate(folio);
    return 0;
}

static int ifs_ofs_ofs_write_end(
    IFS_OFS_AOPS_WRITE_CONTEXT, struct address_space *mapping,
    loff_t position, unsigned int length, unsigned int copied,
    struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct super_block *sb = inode->i_sb;
    const u32 payload = AFFS_SB(sb)->s_data_blksize;

    (void)IFS_OFS_AOPS_WRITE_CONTEXT_ARG;
    const unsigned int from =
        (unsigned int)(position & (PAGE_SIZE - 1));
    const char *data = folio_address(folio);
    u32 written = 0U;
    int error = 0;

    if (copied > length)
        copied = length;

    while (written < copied) {
        const u64 absolute = (u64)position + written;
        const u32 logical = (u32)(absolute / payload);
        const u32 within = (u32)(absolute % payload);
        const u32 chunk = min_t(
            u32, payload - within, copied - written);
        struct buffer_head *bh;
        u32 stored_size;

        error = ifs_ofs_ofs_prepare_block(
            inode, logical, &bh);
        if (error != 0)
            break;

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload) {
            affs_brelse(bh);
            error = -EUCLEAN;
            break;
        }

        memcpy(
            AFFS_DATA(bh) + within,
            data + from + written, chunk);
        if (stored_size < within + chunk)
            AFFS_DATA_HEAD(bh)->size =
                cpu_to_be32(within + chunk);
        affs_fix_checksum(sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        written += chunk;
    }

    if (written != 0U) {
        const u64 end = (u64)position + written;

        if (end > (u64)inode->i_size)
            inode->i_size = (loff_t)end;
        AFFS_I(inode)->mmu_private = inode->i_size;

        if ((AFFS_I(inode)->i_protect & FIBF_ARCHIVED) != 0U) {
            AFFS_I(inode)->i_protect &= ~FIBF_ARCHIVED;
            mark_inode_dirty(inode);
        }
        folio_mark_uptodate(folio);
    }

    folio_unlock(folio);
    folio_put(folio);

    if (written != 0U)
        return (int)written;
    return error;
}

const struct address_space_operations affs_aops_ofs = {
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .read_folio = ifs_ofs_ofs_read_folio,
    .write_begin = ifs_ofs_ofs_write_begin,
    .write_end = ifs_ofs_ofs_write_end,
    .migrate_folio = filemap_migrate_folio,
};

void affs_free_prealloc(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    while (info->i_pa_cnt > 0) {
        const u32 block = info->i_lastalloc + 1U;

        info->i_pa_cnt--;
        info->i_lastalloc = block;
        affs_free_block(inode->i_sb, block);
    }
}

static int ifs_ofs_zero_last_block_tail(
    struct inode *inode, u32 target, u32 kept_blocks)
{
    struct buffer_head *bh;
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    const u32 used = target % payload;
    int result;

    if (target == 0U)
        return 0;

    if (affs_test_opt(AFFS_SB(inode->i_sb)->s_flags, SF_OFS)) {
        const u32 logical_size = used == 0U ? payload : used;

        result = ifs_ofs_ofs_prepare_block(
            inode, kept_blocks - 1U, &bh);
        if (result != 0)
            return result;

        if (logical_size < payload)
            memset(
                AFFS_DATA(bh) + logical_size, 0,
                payload - logical_size);
        AFFS_DATA_HEAD(bh)->size = cpu_to_be32(logical_size);
        AFFS_DATA_HEAD(bh)->next = 0;
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        return 0;
    }

    if (used == 0U)
        return 0;

    result = ifs_ofs_map_to_buffer(
        inode, kept_blocks - 1U, false, false,
        &bh, NULL);
    if (result != 0)
        return result;

    memset(bh->b_data + used, 0, payload - used);
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    return 0;
}

static int ifs_ofs_shrink_file(
    struct inode *inode, u32 target)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    const u32 entries = (u32)AFFS_SB(sb)->s_hashsize;
    const u32 payload = AFFS_SB(sb)->s_data_blksize;
    struct buffer_head *kept_extension;
    u32 kept_blocks;
    u32 kept_extensions;
    u32 last_extension_index;
    u32 first_free_entry;
    u32 next_extension;
    u32 entry;
    int result;

    if (ifs_ofs_file_block_count(
            target, payload, &kept_blocks) != 0 ||
        ifs_ofs_file_extension_count(
            kept_blocks, entries, &kept_extensions) != 0)
        return -EUCLEAN;

    if (kept_blocks > info->i_blkcnt ||
        kept_extensions > info->i_extcnt)
        return -EUCLEAN;

    affs_free_prealloc(inode);
    affs_lock_ext(inode);
    ifs_ofs_clear_extension_cache(inode);

    last_extension_index = kept_extensions - 1U;
    kept_extension = affs_bread(sb, (u32)inode->i_ino);
    if (!kept_extension) {
        result = -EIO;
        goto out_unlock;
    }

    for (entry = 0U; entry < last_extension_index; ++entry) {
        const u32 next =
            be32_to_cpu(AFFS_TAIL(sb, kept_extension)->extension);
        struct buffer_head *next_bh;

        if (next == 0U || !affs_validblock(sb, next)) {
            affs_brelse(kept_extension);
            result = -EUCLEAN;
            goto out_unlock;
        }
        next_bh = affs_bread(sb, next);
        if (!next_bh) {
            affs_brelse(kept_extension);
            result = -EIO;
            goto out_unlock;
        }
        affs_brelse(kept_extension);
        kept_extension = next_bh;
    }

    if (!ifs_ofs_extension_valid(
            inode, kept_extension, last_extension_index)) {
        affs_brelse(kept_extension);
        result = -EUCLEAN;
        goto out_unlock;
    }

    first_free_entry =
        kept_blocks == 0U ? 0U : kept_blocks % entries;
    if (kept_blocks != 0U && first_free_entry == 0U)
        first_free_entry = entries;

    {
        const u32 old_count =
            be32_to_cpu(AFFS_HEAD(kept_extension)->block_count);

        if (old_count > entries || first_free_entry > old_count) {
            affs_brelse(kept_extension);
            result = -EUCLEAN;
            goto out_unlock;
        }

        for (entry = old_count; entry < entries; ++entry) {
            if (AFFS_BLOCK(sb, kept_extension, entry) != 0) {
                affs_brelse(kept_extension);
                result = -EUCLEAN;
                goto out_unlock;
            }
        }

        for (entry = first_free_entry; entry < old_count; ++entry) {
            const u32 physical = be32_to_cpu(
                AFFS_BLOCK(sb, kept_extension, entry));

            if (physical == 0U ||
                !affs_validblock(sb, physical)) {
                affs_brelse(kept_extension);
                result = -EUCLEAN;
                goto out_unlock;
            }
            affs_free_block(sb, physical);
            AFFS_BLOCK(sb, kept_extension, entry) = 0;
        }
    }

    next_extension =
        be32_to_cpu(AFFS_TAIL(sb, kept_extension)->extension);
    AFFS_TAIL(sb, kept_extension)->extension = 0;
    AFFS_HEAD(kept_extension)->block_count =
        cpu_to_be32(
            first_free_entry == entries ? entries :
            first_free_entry);
    if (kept_blocks == 0U)
        AFFS_HEAD(kept_extension)->first_data = 0;
    affs_fix_checksum(sb, kept_extension);
    mark_buffer_dirty_inode(kept_extension, inode);
    affs_brelse(kept_extension);

    while (next_extension != 0U) {
        struct buffer_head *bh;
        u32 following;

        if (!affs_validblock(sb, next_extension)) {
            result = -EUCLEAN;
            goto out_unlock;
        }

        bh = affs_bread(sb, next_extension);
        if (!bh) {
            result = -EIO;
            goto out_unlock;
        }

        if (!ifs_ofs_extension_valid(
                inode, bh, kept_extensions)) {
            affs_brelse(bh);
            result = -EUCLEAN;
            goto out_unlock;
        }

        following = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
        {
            const u32 count =
                be32_to_cpu(AFFS_HEAD(bh)->block_count);

            if (count > entries) {
                affs_brelse(bh);
                result = -EUCLEAN;
                goto out_unlock;
            }

            for (entry = count; entry < entries; ++entry) {
                if (AFFS_BLOCK(sb, bh, entry) != 0) {
                    affs_brelse(bh);
                    result = -EUCLEAN;
                    goto out_unlock;
                }
            }

            for (entry = 0U; entry < count; ++entry) {
                const u32 physical =
                    be32_to_cpu(AFFS_BLOCK(sb, bh, entry));

                if (physical == 0U ||
                    !affs_validblock(sb, physical)) {
                    affs_brelse(bh);
                    result = -EUCLEAN;
                    goto out_unlock;
                }
                affs_free_block(sb, physical);
            }
        }

        affs_brelse(bh);
        affs_free_block(sb, next_extension);
        next_extension = following;
        kept_extensions++;
    }

    info->i_blkcnt = kept_blocks;
    if (ifs_ofs_file_extension_count(
            kept_blocks, entries, &info->i_extcnt) != 0) {
        result = -EUCLEAN;
        goto out_unlock;
    }
    info->mmu_private = target;
    result = 0;

out_unlock:
    affs_unlock_ext(inode);
    if (result == 0)
        result = ifs_ofs_zero_last_block_tail(
            inode, target, kept_blocks);
    return result;
}

int affs_truncate(struct inode *inode)
{
    const loff_t requested = inode->i_size;
    const loff_t previous = AFFS_I(inode)->mmu_private;
    int result;

    if (requested < 0 || requested > U32_MAX) {
        inode->i_size = previous;
        return -EFBIG;
    }

    if (requested == previous)
        return 0;

    if (requested > previous) {
        if (affs_test_opt(AFFS_SB(inode->i_sb)->s_flags, SF_OFS))
            result = ifs_ofs_zero_extend_ofs(
                inode, (u32)requested);
        else
            result = ifs_ofs_zero_extend_ffs(
                inode, (u32)requested);
    } else {
        result = ifs_ofs_shrink_file(
            inode, (u32)requested);
    }

    if (result != 0) {
        if (requested > previous) {
            inode->i_size = previous;
            (void)ifs_ofs_shrink_file(
                inode, (u32)previous);
        }
        inode->i_size = previous;
        AFFS_I(inode)->mmu_private = previous;
        truncate_pagecache(inode, previous);
        affs_warning(
            inode->i_sb, "affs_truncate",
            "Could not resize inode %lu: %d",
            inode->i_ino, result);
    }

    mark_inode_dirty(inode);
    return result;
}

int affs_file_fsync(
    struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    int result;
    int block_result;

    result = file_write_and_wait_range(file, start, end);
    if (result != 0)
        return result;

    inode_lock(inode);
    result = write_inode_now(inode, 0);
    block_result = sync_blockdev(inode->i_sb->s_bdev);
    if (result == 0)
        result = block_result;
    inode_unlock(inode);
    return result;
}

const struct file_operations affs_file_operations = {
    .llseek = generic_file_llseek,
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .open = ifs_ofs_file_open,
    .release = ifs_ofs_file_release,
    .fsync = affs_file_fsync,
    .splice_read = filemap_splice_read,
};

const struct inode_operations affs_file_inode_operations = {
    .setattr = affs_notify_change,
};


/* ===== inode integration ===== */
/*
 * Project-authored Linux inode adapter for Amiga OFS.
 */

#include "linux_adapter.h"

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/gfp.h>

static void ifs_ofs_reset_inode_private(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    atomic_set(&info->i_opencnt, 0);
    info->i_blkcnt = 0U;
    info->i_extcnt = 1U;
    info->i_ext_last = ~1U;
    info->i_protect = 0U;
    info->i_lc = NULL;
    info->i_lc_size = 0U;
    info->i_lc_shift = 0U;
    info->i_lc_mask = 0U;
    info->i_ac = NULL;
    info->i_ext_bh = NULL;
    info->mmu_private = 0;
    info->i_lastalloc = 0U;
    info->i_pa_cnt = 0;
}

static time64_t ifs_ofs_inode_time(const struct affs_date *date)
{
    return (time64_t)be32_to_cpu(date->days) * 86400LL +
           (time64_t)be32_to_cpu(date->mins) * 60LL +
           (time64_t)be32_to_cpu(date->ticks) / 50LL +
           AFFS_EPOCH_DELTA +
           (time64_t)sys_tz.tz_minuteswest * 60LL;
}

struct inode *affs_iget(struct super_block *sb, unsigned long inode_number)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh = NULL;
    struct affs_tail *tail;
    struct inode *inode;
    u32 protection;
    u32 type;
    u16 id;
    time64_t timestamp;

    if (inode_number > U32_MAX ||
        !affs_validblock(sb, (u32)inode_number))
        return ERR_PTR(-ESTALE);

    inode = iget_locked(sb, inode_number);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!IFS_OFS_INODE_IS_NEW(inode))
        return inode;

    bh = affs_bread(sb, (u32)inode_number);
    if (!bh) {
        affs_warning(sb, "affs_iget",
                     "Cannot read inode block %lu", inode_number);
        goto bad_inode;
    }

    if (affs_checksum_block(sb, bh) != 0U ||
        be32_to_cpu(AFFS_HEAD(bh)->ptype) != T_SHORT) {
        affs_warning(sb, "affs_iget",
                     "Invalid checksum or primary type in block %lu",
                     inode_number);
        goto bad_inode;
    }

    tail = AFFS_TAIL(sb, bh);
    protection = be32_to_cpu(tail->protect);
    type = be32_to_cpu(tail->stype);

    inode->i_size = 0;
    set_nlink(inode, 1);
    inode->i_mode = 0;
    ifs_ofs_reset_inode_private(inode);
    AFFS_I(inode)->i_protect = protection;

    if (affs_test_opt(sbi->s_flags, SF_SETMODE))
        inode->i_mode = sbi->s_mode;
    else
        inode->i_mode = affs_prot_to_mode(protection);

    id = be16_to_cpu(tail->uid);
    if (id == 0U || affs_test_opt(sbi->s_flags, SF_SETUID))
        inode->i_uid = sbi->s_uid;
    else if (id == 0xffffU && affs_test_opt(sbi->s_flags, SF_MUFS))
        i_uid_write(inode, 0U);
    else
        i_uid_write(inode, id);

    id = be16_to_cpu(tail->gid);
    if (id == 0U || affs_test_opt(sbi->s_flags, SF_SETGID))
        inode->i_gid = sbi->s_gid;
    else if (id == 0xffffU && affs_test_opt(sbi->s_flags, SF_MUFS))
        i_gid_write(inode, 0U);
    else
        i_gid_write(inode, id);

    switch (type) {
    case ST_ROOT:
        inode->i_uid = sbi->s_uid;
        inode->i_gid = sbi->s_gid;
        if (!affs_test_opt(sbi->s_flags, SF_SETMODE))
            inode->i_mode = S_IRUGO | S_IXUGO | S_IWUSR;
        fallthrough;

    case ST_USERDIR:
        if (type == ST_USERDIR ||
            affs_test_opt(sbi->s_flags, SF_SETMODE)) {
            if ((inode->i_mode & S_IRUSR) != 0)
                inode->i_mode |= S_IXUSR;
            if ((inode->i_mode & S_IRGRP) != 0)
                inode->i_mode |= S_IXGRP;
            if ((inode->i_mode & S_IROTH) != 0)
                inode->i_mode |= S_IXOTH;
        }
        inode->i_mode |= S_IFDIR;
        inode->i_op = &affs_dir_inode_operations;
        inode->i_fop = &affs_dir_operations;
        break;

    case ST_LINKDIR:
        inode->i_mode |= S_IFDIR;
        break;

    case ST_LINKFILE:
        affs_warning(sb, "affs_iget",
                     "Unexpected hard-link header %lu", inode_number);
        goto bad_inode;

    case ST_FILE: {
        const u32 size = be32_to_cpu(tail->size);

        if (sbi->s_data_blksize == 0U || sbi->s_hashsize <= 0)
            goto bad_inode;

        inode->i_mode |= S_IFREG;
        inode->i_size = size;
        AFFS_I(inode)->mmu_private = size;

        if (ifs_ofs_file_block_count(
                size, sbi->s_data_blksize,
                &AFFS_I(inode)->i_blkcnt) != 0 ||
            ifs_ofs_file_extension_count(
                AFFS_I(inode)->i_blkcnt, (u32)sbi->s_hashsize,
                &AFFS_I(inode)->i_extcnt) != 0)
            goto bad_inode;

        if (tail->link_chain != 0)
            set_nlink(inode, 2);

        inode->i_mapping->a_ops =
            affs_test_opt(sbi->s_flags, SF_OFS) ?
            &affs_aops_ofs : &affs_aops;
        inode->i_op = &affs_file_inode_operations;
        inode->i_fop = &affs_file_operations;
        break;
    }

    case ST_SOFTLINK: {
        const size_t capacity = (size_t)sbi->s_hashsize * sizeof(u32);
        const size_t length =
            strnlen((const char *)AFFS_HEAD(bh)->table, capacity);

        if (length == capacity) {
            affs_warning(sb, "affs_iget",
                         "Unterminated symbolic link in block %lu",
                         inode_number);
            goto bad_inode;
        }

        inode->i_size = (loff_t)length;
        inode->i_mode |= S_IFLNK;
        inode_nohighmem(inode);
        inode->i_op = &affs_symlink_inode_operations;
        inode->i_data.a_ops = &affs_symlink_aops;
        break;
    }

    default:
        affs_warning(sb, "affs_iget",
                     "Unsupported secondary type %u in block %lu",
                     type, inode_number);
        goto bad_inode;
    }

    timestamp = ifs_ofs_inode_time(&tail->change);
    inode_set_ctime(inode, timestamp, 0);
    inode_set_atime(inode, timestamp, 0);
    inode_set_mtime(inode, timestamp, 0);

    affs_brelse(bh);
    unlock_new_inode(inode);
    return inode;

bad_inode:
    affs_brelse(bh);
    iget_failed(inode);
    return ERR_PTR(-EIO);
}

int affs_write_inode(struct inode *inode, struct writeback_control *writeback)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh;
    struct affs_tail *tail;
    uid_t uid;
    gid_t gid;

    if (inode->i_nlink == 0)
        return 0;

    if (inode->i_size < 0 || inode->i_size > U32_MAX)
        return -EFBIG;

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh) {
        affs_error(sb, "affs_write_inode",
                   "Cannot read inode block %lu", inode->i_ino);
        return -EIO;
    }

    tail = AFFS_TAIL(sb, bh);
    if (be32_to_cpu(tail->stype) == ST_ROOT) {
        affs_secs_to_datestamp(
            inode_get_mtime_sec(inode),
            &AFFS_ROOT_TAIL(sb, bh)->root_change);
    } else {
        tail->protect = cpu_to_be32(AFFS_I(inode)->i_protect);
        tail->size = cpu_to_be32((u32)inode->i_size);
        affs_secs_to_datestamp(
            inode_get_mtime_sec(inode), &tail->change);

        if (inode->i_ino != sbi->s_root_block) {
            uid = i_uid_read(inode);
            gid = i_gid_read(inode);

            if (affs_test_opt(sbi->s_flags, SF_MUFS)) {
                if (uid == 0U || uid == 0xffffU)
                    uid ^= 0xffffU;
                if (gid == 0U || gid == 0xffffU)
                    gid ^= 0xffffU;
            }

            if (!affs_test_opt(sbi->s_flags, SF_SETUID))
                tail->uid = cpu_to_be16((u16)uid);
            if (!affs_test_opt(sbi->s_flags, SF_SETGID))
                tail->gid = cpu_to_be16((u16)gid);
        }
    }

    affs_fix_checksum(sb, bh);
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    affs_free_prealloc(inode);
    return 0;
}

int affs_notify_change(
    struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attributes)
{
    struct inode *inode = d_inode(dentry);
    struct affs_sb_info *sbi = AFFS_SB(inode->i_sb);
    int result;

    result = setattr_prepare(&nop_mnt_idmap, dentry, attributes);
    if (result != 0)
        return result;

    if (((attributes->ia_valid & ATTR_UID) != 0 &&
         affs_test_opt(sbi->s_flags, SF_SETUID)) ||
        ((attributes->ia_valid & ATTR_GID) != 0 &&
         affs_test_opt(sbi->s_flags, SF_SETGID)) ||
        ((attributes->ia_valid & ATTR_MODE) != 0 &&
         (sbi->s_flags &
          (AFFS_MOUNT_SF_SETMODE | AFFS_MOUNT_SF_IMMUTABLE)) != 0UL)) {
        return affs_test_opt(sbi->s_flags, SF_QUIET) ? 0 : -EPERM;
    }

    if ((attributes->ia_valid & ATTR_SIZE) != 0 &&
        attributes->ia_size != i_size_read(inode)) {
        if (attributes->ia_size < 0 || attributes->ia_size > U32_MAX)
            return -EFBIG;

        result = inode_newsize_ok(inode, attributes->ia_size);
        if (result != 0)
            return result;

        truncate_setsize(inode, attributes->ia_size);
        result = affs_truncate(inode);
        if (result != 0)
            return result;
    }

    setattr_copy(&nop_mnt_idmap, inode, attributes);
    if ((attributes->ia_valid & ATTR_MODE) != 0)
        affs_mode_to_prot(inode);

    mark_inode_dirty(inode);
    return 0;
}

void affs_evict_inode(struct inode *inode)
{
    unsigned long cache_page;

    truncate_inode_pages_final(&inode->i_data);

    if (inode->i_nlink == 0) {
        inode->i_size = 0;
        affs_truncate(inode);
    }

    invalidate_inode_buffers(inode);
    clear_inode(inode);
    affs_free_prealloc(inode);

    cache_page = (unsigned long)AFFS_I(inode)->i_lc;
    if (cache_page != 0UL) {
        AFFS_I(inode)->i_lc = NULL;
        AFFS_I(inode)->i_ac = NULL;
        free_page(cache_page);
    }

    affs_brelse(AFFS_I(inode)->i_ext_bh);
    AFFS_I(inode)->i_ext_bh = NULL;
    AFFS_I(inode)->i_ext_last = ~1U;

    if (inode->i_nlink == 0)
        affs_free_block(inode->i_sb, (u32)inode->i_ino);
}

struct inode *affs_new_inode(struct inode *dir)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    struct buffer_head *bh;
    u32 block;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    block = affs_alloc_block(dir, (u32)dir->i_ino);
    if (block == 0U) {
        iput(inode);
        return NULL;
    }

    inode->i_ino = block;
    bh = affs_getzeroblk(sb, block);
    if (!bh) {
        affs_free_block(sb, block);
        iput(inode);
        return NULL;
    }

    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();
    set_nlink(inode, 1);
    simple_inode_init_ts(inode);
    ifs_ofs_reset_inode_private(inode);
    insert_inode_hash(inode);

    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    return inode;
}

int affs_add_entry(
    struct inode *dir, struct inode *inode,
    struct dentry *dentry, s32 type)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *inode_bh = NULL;
    struct buffer_head *entry_bh = NULL;
    __be32 old_link_chain = 0;
    u32 link_block = 0U;
    bool is_link = type == ST_LINKFILE || type == ST_LINKDIR;
    bool chain_published = false;
    int result = -EIO;

    inode_bh = affs_bread(sb, (u32)inode->i_ino);
    if (!inode_bh)
        return -EIO;

    affs_lock_link(inode);

    if (is_link) {
        link_block = affs_alloc_block(dir, (u32)dir->i_ino);
        if (link_block == 0U) {
            result = -ENOSPC;
            goto out_unlock;
        }

        entry_bh = affs_getzeroblk(sb, link_block);
        if (!entry_bh) {
            result = -EIO;
            goto out_unlock;
        }
    } else {
        entry_bh = inode_bh;
    }

    AFFS_HEAD(entry_bh)->ptype = cpu_to_be32(T_SHORT);
    AFFS_HEAD(entry_bh)->key = cpu_to_be32((u32)entry_bh->b_blocknr);
    affs_copy_name(AFFS_TAIL(sb, entry_bh)->name, dentry);
    AFFS_TAIL(sb, entry_bh)->stype = cpu_to_be32(type);
    AFFS_TAIL(sb, entry_bh)->parent = cpu_to_be32((u32)dir->i_ino);

    if (is_link) {
        old_link_chain = AFFS_TAIL(sb, inode_bh)->link_chain;
        AFFS_TAIL(sb, entry_bh)->original =
            cpu_to_be32((u32)inode->i_ino);
        AFFS_TAIL(sb, entry_bh)->link_chain = old_link_chain;

        AFFS_TAIL(sb, inode_bh)->link_chain =
            cpu_to_be32(link_block);
        affs_fix_checksum(sb, inode_bh);
        mark_buffer_dirty_inode(inode_bh, inode);
        chain_published = true;
    }

    affs_fix_checksum(sb, entry_bh);
    mark_buffer_dirty_inode(entry_bh, inode);

    affs_lock_dir(dir);
    result = affs_insert_hash(dir, entry_bh);
    affs_unlock_dir(dir);
    if (result != 0)
        goto rollback;

    dentry->d_fsdata =
        (void *)(unsigned long)entry_bh->b_blocknr;

    if (is_link) {
        set_nlink(inode, 2);
        ihold(inode);
    }

    affs_unlock_link(inode);
    d_instantiate(dentry, inode);

    if (entry_bh != inode_bh)
        affs_brelse(entry_bh);
    affs_brelse(inode_bh);
    return 0;

rollback:
    if (chain_published) {
        AFFS_TAIL(sb, inode_bh)->link_chain = old_link_chain;
        affs_fix_checksum(sb, inode_bh);
        mark_buffer_dirty_inode(inode_bh, inode);
    }

out_unlock:
    if (is_link && link_block != 0U)
        affs_free_block(sb, link_block);

    affs_unlock_link(inode);
    if (entry_bh && entry_bh != inode_bh)
        affs_brelse(entry_bh);
    affs_brelse(inode_bh);
    return result;
}

