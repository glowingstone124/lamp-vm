#include "../include/kernel/blk.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/fs_ext4.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/syscall.h"

enum {
    EXT4_FS_LBA_BASE = 2048u,
    EXT4_SUPER_SIZE = 1024u,
    EXT4_SUPER_LBA_OFF = 2u, /* +1024 bytes from FS start */
    EXT4_SUPER_LBA_COUNT = 2u,
    EXT4_SUPER_MAGIC = 0xEF53u,
    EXT4_EXTENT_MAGIC = 0xF30Au,
    EXT4_EXTENTS_FL = 0x00080000u,
    EXT4_INCOMPAT_64BIT = 0x00000080u,
    EXT4_S_IFMT = 0xF000u,
    EXT4_S_IFLNK = 0xA000u,
    EXT4_S_IFREG = 0x8000u,
    EXT4_S_IFDIR = 0x4000u,
    EXT4_ROOT_INO = 2u,
    EXT4_MAX_NAME = 255u,
    EXT4_PATH_CAP = 256u,
    EXT4_SYMLINK_INLINE_MAX = 60u,
    EXT4_SYMLINK_FOLLOW_MAX = 8u
};

typedef struct ext4_state {
    uint32_t probe_done;
    uint32_t mounted;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t blocks_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t group_count;
    uint32_t desc_size;
    uint32_t feature_incompat;
} ext4_state_t;

enum {
    EXT4_BLOCK_CACHE_SLOTS = 8u
};

typedef struct ext4_block_cache_entry {
    uint32_t valid;
    uint32_t fs_block;
    uint8_t data[4096];
} ext4_block_cache_entry_t;

static ext4_state_t g_ext4;
static spinlock_t g_ext4_lock;
static sched_waitq_t g_ext4_waitq;
static volatile uint32_t g_ext4_lock_held;

static uint8_t g_super[EXT4_SUPER_SIZE];
static uint8_t g_io_block[4096];
static uint8_t g_tree_block[4096];
static uint8_t g_inode_buf[512];
static uint8_t g_desc_buf[64];
static ext4_block_cache_entry_t g_block_cache[EXT4_BLOCK_CACHE_SLOTS];

static int ext4_probe_fail_nosys(const char *reason, uint32_t v0, uint32_t v1) {
    klog_begin(KLOG_LEVEL_ERROR, "ext4");
    klog_puts("probe nosys ");
    klog_puts(reason ? reason : "?");
    klog_puts(" v0=");
    klog_hex32(v0);
    klog_puts(" v1=");
    klog_hex32(v1);
    klog_end();
    g_ext4.probe_done = 1u;
    g_ext4.mounted = 0u;
    return FS_ERR_NOSYS;
}

static inline uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t mem_eq_u8(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return 0u;
        }
    }
    return 1u;
}

static inline void mem_copy_u8(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static uint32_t ext4_str_len_cap(const char *s, uint32_t cap) {
    uint32_t n = 0u;
    if (!s) {
        return 0u;
    }
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

static int ext4_path_append_char(char *dst, uint32_t *len, char c) {
    if (!dst || !len || *len + 1u >= EXT4_PATH_CAP) {
        return FS_ERR_NAMETOOLONG;
    }
    dst[*len] = c;
    *len += 1u;
    dst[*len] = '\0';
    return 0;
}

static int ext4_path_append_component(char *dst, uint32_t *len, const char *comp, uint32_t comp_len) {
    if (!dst || !len || !comp) {
        return FS_ERR_INVAL;
    }
    if (comp_len == 0u || (comp_len == 1u && comp[0] == '.')) {
        return 0;
    }
    if (comp_len == 2u && comp[0] == '.' && comp[1] == '.') {
        if (*len > 1u) {
            while (*len > 1u && dst[*len - 1u] != '/') {
                *len -= 1u;
            }
            if (*len > 1u) {
                *len -= 1u;
            }
            dst[*len] = '\0';
        }
        return 0;
    }
    if (*len > 1u) {
        int rc = ext4_path_append_char(dst, len, '/');
        if (rc != 0) {
            return rc;
        }
    }
    for (uint32_t i = 0u; i < comp_len; i++) {
        int rc = ext4_path_append_char(dst, len, comp[i]);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int ext4_path_normalize_absolute(const char *input, char *out) {
    uint32_t in_len;
    uint32_t out_len = 1u;
    uint32_t pos = 1u;
    if (!input || !out || input[0] != '/') {
        return FS_ERR_INVAL;
    }
    in_len = ext4_str_len_cap(input, EXT4_PATH_CAP);
    if (in_len == 0u || in_len >= EXT4_PATH_CAP) {
        return FS_ERR_NAMETOOLONG;
    }
    out[0] = '/';
    out[1] = '\0';
    while (pos <= in_len) {
        uint32_t start;
        uint32_t comp_len;
        while (pos < in_len && input[pos] == '/') {
            pos++;
        }
        start = pos;
        while (pos < in_len && input[pos] != '/') {
            pos++;
        }
        comp_len = pos - start;
        {
            int rc = ext4_path_append_component(out, &out_len, &input[start], comp_len);
            if (rc != 0) {
                return rc;
            }
        }
        if (pos >= in_len) {
            break;
        }
    }
    return 0;
}

static int ext4_path_copy_prefix(char *dst, uint32_t *len, const char *src, uint32_t end) {
    if (!dst || !len || !src || src[0] != '/') {
        return FS_ERR_INVAL;
    }
    dst[0] = '/';
    dst[1] = '\0';
    *len = 1u;
    for (uint32_t i = 1u; i < end && src[i] != '\0'; i++) {
        int rc = ext4_path_append_char(dst, len, src[i]);
        if (rc != 0) {
            return rc;
        }
    }
    while (*len > 1u && dst[*len - 1u] == '/') {
        *len -= 1u;
        dst[*len] = '\0';
    }
    return 0;
}

static void ext4_block_cache_reset(void) {
    for (uint32_t i = 0u; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        g_block_cache[i].valid = 0u;
        g_block_cache[i].fs_block = 0u;
    }
}

static ext4_block_cache_entry_t *ext4_block_cache_slot(uint32_t fs_block) {
    return &g_block_cache[fs_block % EXT4_BLOCK_CACHE_SLOTS];
}

static int ext4_block_cache_read(uint32_t fs_block, uint8_t *dst) {
    ext4_block_cache_entry_t *entry;
    if (!dst || g_ext4.block_size == 0u || g_ext4.block_size > 4096u) {
        return 0;
    }
    entry = ext4_block_cache_slot(fs_block);
    if (entry->valid == 0u || entry->fs_block != fs_block) {
        return 0;
    }
    mem_copy_u8(dst, &entry->data[0], g_ext4.block_size);
    return 1;
}

static void ext4_block_cache_store(uint32_t fs_block, const uint8_t *src) {
    ext4_block_cache_entry_t *entry;
    if (!src || g_ext4.block_size == 0u || g_ext4.block_size > 4096u) {
        return;
    }
    entry = ext4_block_cache_slot(fs_block);
    mem_copy_u8(&entry->data[0], src, g_ext4.block_size);
    entry->fs_block = fs_block;
    entry->valid = 1u;
}

static void ext4_mutex_lock(void) {
    for (;;) {
        spinlock_lock(&g_ext4_lock);
        if (g_ext4_lock_held == 0u) {
            g_ext4_lock_held = 1u;
            spinlock_unlock(&g_ext4_lock);
            return;
        }
        sched_waitq_sleep(&g_ext4_waitq, 0u);
        spinlock_unlock(&g_ext4_lock);
        sched_block_until_runnable();
    }
}

static void ext4_mutex_unlock(void) {
    uint32_t wake = 0u;
    spinlock_lock(&g_ext4_lock);
    if (g_ext4_lock_held != 0u) {
        g_ext4_lock_held = 0u;
        wake = 1u;
    }
    spinlock_unlock(&g_ext4_lock);
    if (wake) {
        sched_waitq_wake_one(&g_ext4_waitq);
    }
}

static int ext4_read_block_locked(uint32_t fs_block, uint8_t *dst) {
    uint32_t lba = EXT4_FS_LBA_BASE + fs_block * g_ext4.sectors_per_block;
    if (!dst || g_ext4.block_size == 0u || g_ext4.block_size > sizeof(g_io_block)) {
        return FS_ERR_IO;
    }
    if (ext4_block_cache_read(fs_block, dst)) {
        return 0;
    }
    if (blk_read_sectors(lba, g_ext4.sectors_per_block, (uint32_t)(uintptr_t)dst) != BLK_OK) {
        return FS_ERR_IO;
    }
    ext4_block_cache_store(fs_block, dst);
    return 0;
}

static int ext4_write_block_locked(uint32_t fs_block, const uint8_t *src) {
    uint32_t lba = EXT4_FS_LBA_BASE + fs_block * g_ext4.sectors_per_block;
    if (!src || g_ext4.block_size == 0u || g_ext4.block_size > sizeof(g_io_block)) {
        return FS_ERR_IO;
    }
    if (blk_write_sectors(lba, g_ext4.sectors_per_block, (uint32_t)(uintptr_t)src) != BLK_OK) {
        return FS_ERR_IO;
    }
    ext4_block_cache_store(fs_block, src);
    return 0;
}

static int ext4_read_group_desc_locked(uint32_t group, uint32_t *out_block_bitmap, uint32_t *out_inode_table) {
    uint32_t gdt_first_block;
    uint32_t desc_off;
    uint32_t block;
    uint32_t in_block_off;
    uint32_t first_part;
    uint32_t block_bitmap_lo;
    uint32_t inode_table_lo;
    uint32_t block_bitmap_hi = 0u;
    uint32_t inode_table_hi = 0u;

    if ((!out_inode_table && !out_block_bitmap) || group >= g_ext4.group_count) {
        return FS_ERR_INVAL;
    }
    if (g_ext4.desc_size < 32u || g_ext4.desc_size > sizeof(g_desc_buf)) {
        return FS_ERR_NOSYS;
    }

    gdt_first_block = (g_ext4.block_size == 1024u) ? 2u : 1u;
    desc_off = group * g_ext4.desc_size;
    block = gdt_first_block + (desc_off / g_ext4.block_size);
    in_block_off = desc_off % g_ext4.block_size;

    if (ext4_read_block_locked(block, g_io_block) != 0) {
        return FS_ERR_IO;
    }
    if (in_block_off + g_ext4.desc_size <= g_ext4.block_size) {
        for (uint32_t i = 0u; i < g_ext4.desc_size; i++) {
            g_desc_buf[i] = g_io_block[in_block_off + i];
        }
    } else {
        first_part = g_ext4.block_size - in_block_off;
        for (uint32_t i = 0u; i < first_part; i++) {
            g_desc_buf[i] = g_io_block[in_block_off + i];
        }
        if (ext4_read_block_locked(block + 1u, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        for (uint32_t i = first_part; i < g_ext4.desc_size; i++) {
            g_desc_buf[i] = g_io_block[i - first_part];
        }
    }

    block_bitmap_lo = rd_le32(&g_desc_buf[0x00u]);
    inode_table_lo = rd_le32(&g_desc_buf[0x08u]);
    if ((g_ext4.feature_incompat & EXT4_INCOMPAT_64BIT) != 0u && g_ext4.desc_size >= 64u) {
        block_bitmap_hi = rd_le32(&g_desc_buf[0x20u]);
        inode_table_hi = rd_le32(&g_desc_buf[0x28u]);
        if (block_bitmap_hi != 0u) {
            return FS_ERR_NOSYS;
        }
        if (inode_table_hi != 0u) {
            return FS_ERR_NOSYS;
        }
    }
    if (out_block_bitmap) {
        *out_block_bitmap = block_bitmap_lo;
    }
    if (out_inode_table) {
        *out_inode_table = inode_table_lo;
    }
    return 0;
}

static int ext4_read_inode_locked(uint32_t inode_no, uint8_t *out_inode) {
    uint32_t group;
    uint32_t index;
    uint32_t inode_table;
    uint32_t inode_off;
    uint32_t block;
    uint32_t off;
    if (!out_inode || inode_no == 0u || g_ext4.inodes_per_group == 0u) {
        return FS_ERR_INVAL;
    }

    group = (inode_no - 1u) / g_ext4.inodes_per_group;
    index = (inode_no - 1u) % g_ext4.inodes_per_group;
    if (group >= g_ext4.group_count) {
        return FS_ERR_NOENT;
    }
    if (ext4_read_group_desc_locked(group, 0, &inode_table) != 0) {
        return FS_ERR_IO;
    }

    inode_off = index * g_ext4.inode_size;
    block = inode_table + (inode_off / g_ext4.block_size);
    off = inode_off % g_ext4.block_size;

    if (ext4_read_block_locked(block, g_io_block) != 0) {
        return FS_ERR_IO;
    }
    if (off + g_ext4.inode_size <= g_ext4.block_size) {
        for (uint32_t i = 0u; i < g_ext4.inode_size; i++) {
            out_inode[i] = g_io_block[off + i];
        }
    } else {
        uint32_t first_part = g_ext4.block_size - off;
        for (uint32_t i = 0u; i < first_part; i++) {
            out_inode[i] = g_io_block[off + i];
        }
        if (ext4_read_block_locked(block + 1u, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        for (uint32_t i = first_part; i < g_ext4.inode_size; i++) {
            out_inode[i] = g_io_block[i - first_part];
        }
    }
    return 0;
}

static int ext4_write_inode_locked(uint32_t inode_no, const uint8_t *inode_data) {
    uint32_t group;
    uint32_t index;
    uint32_t inode_table;
    uint32_t inode_off;
    uint32_t block;
    uint32_t off;
    if (!inode_data || inode_no == 0u || g_ext4.inodes_per_group == 0u) {
        return FS_ERR_INVAL;
    }

    group = (inode_no - 1u) / g_ext4.inodes_per_group;
    index = (inode_no - 1u) % g_ext4.inodes_per_group;
    if (group >= g_ext4.group_count) {
        return FS_ERR_NOENT;
    }
    if (ext4_read_group_desc_locked(group, 0, &inode_table) != 0) {
        return FS_ERR_IO;
    }

    inode_off = index * g_ext4.inode_size;
    block = inode_table + (inode_off / g_ext4.block_size);
    off = inode_off % g_ext4.block_size;

    if (ext4_read_block_locked(block, g_io_block) != 0) {
        return FS_ERR_IO;
    }
    if (off + g_ext4.inode_size <= g_ext4.block_size) {
        for (uint32_t i = 0u; i < g_ext4.inode_size; i++) {
            g_io_block[off + i] = inode_data[i];
        }
        if (ext4_write_block_locked(block, g_io_block) != 0) {
            return FS_ERR_IO;
        }
    } else {
        uint32_t first_part = g_ext4.block_size - off;
        for (uint32_t i = 0u; i < first_part; i++) {
            g_io_block[off + i] = inode_data[i];
        }
        if (ext4_write_block_locked(block, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        if (ext4_read_block_locked(block + 1u, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        for (uint32_t i = first_part; i < g_ext4.inode_size; i++) {
            g_io_block[i - first_part] = inode_data[i];
        }
        if (ext4_write_block_locked(block + 1u, g_io_block) != 0) {
            return FS_ERR_IO;
        }
    }
    return 0;
}

static inline uint16_t ext4_inode_mode(const uint8_t *inode) {
    return rd_le16(&inode[0x00u]);
}

static inline uint16_t ext4_inode_links_count(const uint8_t *inode) {
    return rd_le16(&inode[0x1Au]);
}

static inline uint32_t ext4_inode_flags(const uint8_t *inode) {
    return rd_le32(&inode[0x20u]);
}

static int ext4_inode_size32(const uint8_t *inode, uint32_t *out_size) {
    uint32_t lo;
    uint32_t hi;
    uint16_t type;
    if (!inode || !out_size) {
        return FS_ERR_INVAL;
    }
    lo = rd_le32(&inode[0x04u]);
    hi = rd_le32(&inode[0x6Cu]);
    type = ext4_inode_mode(inode) & EXT4_S_IFMT;
    if (type == EXT4_S_IFREG && hi != 0u) {
        return FS_ERR_NOSYS;
    }
    *out_size = lo;
    return 0;
}

static int ext4_set_inode_size_locked(uint32_t inode_no, uint32_t new_size) {
    if (ext4_read_inode_locked(inode_no, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }
    wr_le32(&g_inode_buf[0x04u], new_size);
    wr_le32(&g_inode_buf[0x6Cu], 0u);
    if (ext4_write_inode_locked(inode_no, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }
    return 0;
}

static int ext4_extent_map_locked(const uint8_t *extent_root, uint32_t lblock, uint32_t *out_pblock) {
    const uint8_t *node = extent_root;
    uint32_t depth_guard = 0u;
    if (!extent_root || !out_pblock) {
        return FS_ERR_INVAL;
    }

    for (;;) {
        uint16_t magic = rd_le16(&node[0x00u]);
        uint16_t entries = rd_le16(&node[0x02u]);
        uint16_t depth = rd_le16(&node[0x06u]);
        const uint8_t *ent = &node[0x0Cu];
        uint32_t chosen = 0xFFFFFFFFu;

        if (magic != EXT4_EXTENT_MAGIC) {
            return FS_ERR_IO;
        }
        if (entries == 0u) {
            return FS_ERR_NOENT;
        }
        if (depth > 5u || depth_guard++ > 6u) {
            return FS_ERR_NOSYS;
        }

        if (depth == 0u) {
            for (uint32_t i = 0u; i < (uint32_t)entries; i++) {
                uint32_t ee_block = rd_le32(&ent[i * 12u + 0u]);
                uint16_t ee_len_raw = rd_le16(&ent[i * 12u + 4u]);
                uint16_t ee_start_hi = rd_le16(&ent[i * 12u + 6u]);
                uint32_t ee_start_lo = rd_le32(&ent[i * 12u + 8u]);
                uint32_t ee_len = (uint32_t)(ee_len_raw & 0x7FFFu);
                uint32_t ee_end = ee_block + ee_len;
                if (ee_len == 0u) {
                    continue;
                }
                if (lblock >= ee_block && lblock < ee_end) {
                    if (ee_start_hi != 0u) {
                        return FS_ERR_NOSYS;
                    }
                    *out_pblock = ee_start_lo + (lblock - ee_block);
                    return 0;
                }
            }
            return FS_ERR_NOENT;
        }

        for (uint32_t i = 0u; i < (uint32_t)entries; i++) {
            uint32_t ei_block = rd_le32(&ent[i * 12u + 0u]);
            if (ei_block > lblock) {
                break;
            }
            chosen = i;
        }
        if (chosen == 0xFFFFFFFFu) {
            return FS_ERR_NOENT;
        }
        {
            uint32_t ei_leaf_lo = rd_le32(&ent[chosen * 12u + 4u]);
            uint16_t ei_leaf_hi = rd_le16(&ent[chosen * 12u + 8u]);
            if (ei_leaf_hi != 0u) {
                return FS_ERR_NOSYS;
            }
            if (ext4_read_block_locked(ei_leaf_lo, g_tree_block) != 0) {
                return FS_ERR_IO;
            }
            node = g_tree_block;
        }
    }
}

static int ext4_legacy_map_locked(const uint8_t *inode, uint32_t lblock, uint32_t *out_pblock) {
    const uint8_t *i_block;
    uint32_t ptrs_per_block;
    uint32_t pblock;

    if (!inode || !out_pblock) {
        return FS_ERR_INVAL;
    }
    ptrs_per_block = g_ext4.block_size / 4u;
    if (ptrs_per_block == 0u) {
        return FS_ERR_NOSYS;
    }
    i_block = &inode[0x28u];

    if (lblock < 12u) {
        pblock = rd_le32(&i_block[lblock * 4u]);
        if (pblock == 0u) {
            return FS_ERR_NOENT;
        }
        *out_pblock = pblock;
        return 0;
    }
    lblock -= 12u;

    if (lblock < ptrs_per_block) {
        uint32_t ind = rd_le32(&i_block[12u * 4u]);
        if (ind == 0u) {
            return FS_ERR_NOENT;
        }
        if (ext4_read_block_locked(ind, g_tree_block) != 0) {
            return FS_ERR_IO;
        }
        pblock = rd_le32(&g_tree_block[lblock * 4u]);
        if (pblock == 0u) {
            return FS_ERR_NOENT;
        }
        *out_pblock = pblock;
        return 0;
    }
    lblock -= ptrs_per_block;

    if (lblock < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = rd_le32(&i_block[13u * 4u]);
        uint32_t l1 = lblock / ptrs_per_block;
        uint32_t l2 = lblock % ptrs_per_block;
        uint32_t ind;
        if (dind == 0u) {
            return FS_ERR_NOENT;
        }
        if (ext4_read_block_locked(dind, g_tree_block) != 0) {
            return FS_ERR_IO;
        }
        ind = rd_le32(&g_tree_block[l1 * 4u]);
        if (ind == 0u) {
            return FS_ERR_NOENT;
        }
        if (ext4_read_block_locked(ind, g_tree_block) != 0) {
            return FS_ERR_IO;
        }
        pblock = rd_le32(&g_tree_block[l2 * 4u]);
        if (pblock == 0u) {
            return FS_ERR_NOENT;
        }
        *out_pblock = pblock;
        return 0;
    }

    return FS_ERR_NOSYS;
}

static int ext4_inode_map_block_locked(const uint8_t *inode, uint32_t lblock, uint32_t *out_pblock) {
    if (!inode || !out_pblock) {
        return FS_ERR_INVAL;
    }
    if ((ext4_inode_flags(inode) & EXT4_EXTENTS_FL) != 0u) {
        return ext4_extent_map_locked(&inode[0x28u], lblock, out_pblock);
    }
    return ext4_legacy_map_locked(inode, lblock, out_pblock);
}

static int ext4_alloc_block_locked(uint32_t inode_no, uint32_t *out_pblock) {
    uint32_t inode_group;
    if (!out_pblock || g_ext4.blocks_count == 0u || g_ext4.blocks_per_group == 0u) {
        return FS_ERR_INVAL;
    }
    inode_group = (inode_no > 0u && g_ext4.inodes_per_group != 0u)
                      ? ((inode_no - 1u) / g_ext4.inodes_per_group)
                      : 0u;
    if (inode_group >= g_ext4.group_count) {
        inode_group = 0u;
    }

    for (uint32_t pass = 0u; pass < g_ext4.group_count; pass++) {
        uint32_t group = (inode_group + pass) % g_ext4.group_count;
        uint32_t bitmap_block = 0u;
        uint32_t group_first = g_ext4.first_data_block + group * g_ext4.blocks_per_group;
        uint32_t group_end = group_first + g_ext4.blocks_per_group;
        uint32_t group_blocks;
        if (group_first >= g_ext4.blocks_count) {
            continue;
        }
        if (group_end > g_ext4.blocks_count) {
            group_end = g_ext4.blocks_count;
        }
        group_blocks = group_end - group_first;
        if (group_blocks == 0u) {
            continue;
        }
        if (ext4_read_group_desc_locked(group, &bitmap_block, 0) != 0) {
            continue;
        }
        if (ext4_read_block_locked(bitmap_block, g_io_block) != 0) {
            continue;
        }
        for (uint32_t bit = 0u; bit < group_blocks; bit++) {
            uint32_t byte_idx = bit >> 3;
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            if (byte_idx >= g_ext4.block_size) {
                break;
            }
            if ((g_io_block[byte_idx] & mask) == 0u) {
                g_io_block[byte_idx] = (uint8_t)(g_io_block[byte_idx] | mask);
                if (ext4_write_block_locked(bitmap_block, g_io_block) != 0) {
                    return FS_ERR_IO;
                }
                *out_pblock = group_first + bit;
                return 0;
            }
        }
    }
    return FS_ERR_NOSPC;
}

static int ext4_extent_append_block_locked(uint8_t *extent_root, uint32_t lblock, uint32_t pblock) {
    uint16_t magic;
    uint16_t entries;
    uint16_t max_entries;
    uint16_t depth;
    uint8_t *ent;
    if (!extent_root || pblock >= g_ext4.blocks_count) {
        return FS_ERR_INVAL;
    }
    magic = rd_le16(&extent_root[0x00u]);
    entries = rd_le16(&extent_root[0x02u]);
    max_entries = rd_le16(&extent_root[0x04u]);
    depth = rd_le16(&extent_root[0x06u]);
    if (magic != EXT4_EXTENT_MAGIC) {
        return FS_ERR_IO;
    }
    if (depth != 0u) {
        return FS_ERR_NOSYS;
    }
    if (entries > max_entries || max_entries == 0u) {
        return FS_ERR_IO;
    }
    ent = &extent_root[0x0Cu];
    if (entries == 0u) {
        wr_le32(&ent[0x00u], lblock);
        wr_le16(&ent[0x04u], 1u);
        wr_le16(&ent[0x06u], 0u);
        wr_le32(&ent[0x08u], pblock);
        wr_le16(&extent_root[0x02u], 1u);
        return 0;
    }
    {
        uint32_t last_idx = (uint32_t)entries - 1u;
        uint8_t *last = &ent[last_idx * 12u];
        uint32_t ee_block = rd_le32(&last[0x00u]);
        uint16_t ee_len_raw = rd_le16(&last[0x04u]);
        uint16_t ee_start_hi = rd_le16(&last[0x06u]);
        uint32_t ee_start_lo = rd_le32(&last[0x08u]);
        uint32_t ee_len = (uint32_t)(ee_len_raw & 0x7FFFu);
        uint32_t ee_end = ee_block + ee_len;
        if ((ee_len_raw & 0x8000u) != 0u || ee_len == 0u || ee_start_hi != 0u) {
            return FS_ERR_NOSYS;
        }
        if (lblock != ee_end) {
            return FS_ERR_NOSYS;
        }
        if (pblock == ee_start_lo + ee_len && ee_len < 0x7FFFu) {
            wr_le16(&last[0x04u], (uint16_t)(ee_len + 1u));
            return 0;
        }
    }
    if (entries >= max_entries) {
        return FS_ERR_NOSPC;
    }
    {
        uint8_t *dst = &ent[(uint32_t)entries * 12u];
        wr_le32(&dst[0x00u], lblock);
        wr_le16(&dst[0x04u], 1u);
        wr_le16(&dst[0x06u], 0u);
        wr_le32(&dst[0x08u], pblock);
        wr_le16(&extent_root[0x02u], (uint16_t)(entries + 1u));
    }
    return 0;
}

static int ext4_read_inode_data_locked(uint32_t inode_no, uint32_t offset, uint8_t *dst, uint32_t len) {
    uint32_t size;
    uint32_t done = 0u;
    int src;
    if (!dst || len == 0u) {
        return 0;
    }
    if (ext4_read_inode_locked(inode_no, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }

    src = ext4_inode_size32(g_inode_buf, &size);
    if (src != 0) {
        return src;
    }
    if (offset >= size) {
        return 0;
    }
    if (len > (size - offset)) {
        len = size - offset;
    }

    while (done < len) {
        uint32_t logical = (offset + done) / g_ext4.block_size;
        uint32_t in_block = (offset + done) % g_ext4.block_size;
        uint32_t want = len - done;
        uint32_t pblock = 0u;
        int rc;
        if (want > (g_ext4.block_size - in_block)) {
            want = g_ext4.block_size - in_block;
        }

        rc = ext4_inode_map_block_locked(g_inode_buf, logical, &pblock);
        if (rc == FS_ERR_NOENT) {
            for (uint32_t i = 0u; i < want; i++) {
                dst[done + i] = 0u;
            }
            done += want;
            continue;
        }
        if (rc != 0) {
            return (done != 0u) ? (int)done : rc;
        }
        if (ext4_read_block_locked(pblock, g_io_block) != 0) {
            return (done != 0u) ? (int)done : FS_ERR_IO;
        }
        for (uint32_t i = 0u; i < want; i++) {
            dst[done + i] = g_io_block[in_block + i];
        }
        done += want;
    }
    return (int)done;
}

static int ext4_read_symlink_target_locked(uint32_t inode_no, char *dst, uint32_t cap, uint32_t *out_len) {
    uint32_t size;
    uint16_t mode;
    int rc;
    if (!dst || cap == 0u) {
        return FS_ERR_INVAL;
    }
    if (out_len) {
        *out_len = 0u;
    }
    if (ext4_read_inode_locked(inode_no, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }
    mode = ext4_inode_mode(g_inode_buf) & EXT4_S_IFMT;
    if (mode != EXT4_S_IFLNK) {
        return FS_ERR_INVAL;
    }
    rc = ext4_inode_size32(g_inode_buf, &size);
    if (rc != 0) {
        return rc;
    }
    if (size >= cap) {
        return FS_ERR_NAMETOOLONG;
    }
    if (size <= EXT4_SYMLINK_INLINE_MAX && (ext4_inode_flags(g_inode_buf) & EXT4_EXTENTS_FL) == 0u) {
        for (uint32_t i = 0u; i < size; i++) {
            dst[i] = (char)g_inode_buf[0x28u + i];
        }
    } else {
        rc = ext4_read_inode_data_locked(inode_no, 0u, (uint8_t *)dst, size);
        if (rc < 0) {
            return rc;
        }
        if ((uint32_t)rc != size) {
            return FS_ERR_IO;
        }
    }
    dst[size] = '\0';
    if (out_len) {
        *out_len = size;
    }
    return 0;
}

static int ext4_write_inode_data_locked(uint32_t inode_no, uint32_t offset, const uint8_t *src, uint32_t len,
                                        uint32_t *out_new_size) {
    uint32_t size;
    uint32_t cur_size;
    uint32_t done = 0u;
    uint32_t flags;
    uint8_t *extent_root;
    uint32_t inode_dirty = 0u;
    int src_size;

    if (!src || len == 0u) {
        if (out_new_size) {
            *out_new_size = 0u;
        }
        return 0;
    }
    if (ext4_read_inode_locked(inode_no, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }
    flags = ext4_inode_flags(g_inode_buf);
    if ((flags & EXT4_EXTENTS_FL) == 0u) {
        return FS_ERR_NOSYS;
    }

    src_size = ext4_inode_size32(g_inode_buf, &size);
    if (src_size != 0) {
        return src_size;
    }
    cur_size = size;
    extent_root = &g_inode_buf[0x28u];

    /*
     * If write starts beyond EOF and crosses into later blocks, the visible gap
     * includes the tail of current EOF block; force that tail to zero first.
     */
    if (offset > cur_size &&
        (cur_size % g_ext4.block_size) != 0u &&
        (offset / g_ext4.block_size) > (cur_size / g_ext4.block_size)) {
        uint32_t tail_lblock = cur_size / g_ext4.block_size;
        uint32_t tail_off = cur_size % g_ext4.block_size;
        uint32_t tail_pblock = 0u;
        int rc = ext4_extent_map_locked(extent_root, tail_lblock, &tail_pblock);
        if (rc == FS_ERR_NOENT) {
            return FS_ERR_NOSYS;
        }
        if (rc != 0) {
            return rc;
        }
        if (ext4_read_block_locked(tail_pblock, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        for (uint32_t i = tail_off; i < g_ext4.block_size; i++) {
            g_io_block[i] = 0u;
        }
        if (ext4_write_block_locked(tail_pblock, g_io_block) != 0) {
            return FS_ERR_IO;
        }
    }

    while (done < len) {
        uint32_t pos = offset + done;
        uint32_t logical = pos / g_ext4.block_size;
        uint32_t in_block = pos % g_ext4.block_size;
        uint32_t want = len - done;
        uint32_t pblock = 0u;
        int rc;
        uint32_t mapped_new = 0u;

        if (want > (g_ext4.block_size - in_block)) {
            want = g_ext4.block_size - in_block;
        }
        rc = ext4_extent_map_locked(extent_root, logical, &pblock);
        if (rc == FS_ERR_NOENT) {
            uint32_t eof_lblock = (cur_size + g_ext4.block_size - 1u) / g_ext4.block_size;
            if (pos < cur_size || logical != eof_lblock) {
                if (pos < cur_size || logical < eof_lblock) {
                    return (done != 0u) ? (int)done : FS_ERR_NOSYS;
                }
                /* Fill full zero blocks between old EOF and target write block. */
                rc = ext4_alloc_block_locked(inode_no, &pblock);
                if (rc != 0) {
                    return (done != 0u) ? (int)done : rc;
                }
                rc = ext4_extent_append_block_locked(extent_root, eof_lblock, pblock);
                if (rc != 0) {
                    return (done != 0u) ? (int)done : rc;
                }
                inode_dirty = 1u;
                for (uint32_t i = 0u; i < g_ext4.block_size; i++) {
                    g_io_block[i] = 0u;
                }
                if (ext4_write_block_locked(pblock, g_io_block) != 0) {
                    return (done != 0u) ? (int)done : FS_ERR_IO;
                }
                cur_size = (eof_lblock + 1u) * g_ext4.block_size;
                continue;
            }
            rc = ext4_alloc_block_locked(inode_no, &pblock);
            if (rc != 0) {
                return (done != 0u) ? (int)done : rc;
            }
            rc = ext4_extent_append_block_locked(extent_root, logical, pblock);
            if (rc != 0) {
                return (done != 0u) ? (int)done : rc;
            }
            inode_dirty = 1u;
            for (uint32_t i = 0u; i < g_ext4.block_size; i++) {
                g_io_block[i] = 0u;
            }
            mapped_new = 1u;
        }
        if (rc != 0) {
            return (done != 0u) ? (int)done : rc;
        }

        if (!mapped_new && ext4_read_block_locked(pblock, g_io_block) != 0) {
            return (done != 0u) ? (int)done : FS_ERR_IO;
        }

        if (pos > cur_size) {
            uint32_t blk_base = logical * g_ext4.block_size;
            uint32_t z_start = (cur_size > blk_base) ? (cur_size - blk_base) : 0u;
            uint32_t z_end = in_block;
            if (z_end > z_start && z_end <= g_ext4.block_size) {
                for (uint32_t i = z_start; i < z_end; i++) {
                    g_io_block[i] = 0u;
                }
            }
        }

        for (uint32_t i = 0u; i < want; i++) {
            g_io_block[in_block + i] = src[done + i];
        }
        if (ext4_write_block_locked(pblock, g_io_block) != 0) {
            return (done != 0u) ? (int)done : FS_ERR_IO;
        }
        done += want;
        if (offset + done > cur_size) {
            cur_size = offset + done;
        }
    }

    if (done == 0u && len != 0u) {
        return FS_ERR_NOSPC;
    }

    if (cur_size > size) {
        wr_le32(&g_inode_buf[0x04u], cur_size);
        wr_le32(&g_inode_buf[0x6Cu], 0u);
        inode_dirty = 1u;
    }
    if (inode_dirty != 0u) {
        if (ext4_write_inode_locked(inode_no, g_inode_buf) != 0) {
            return (done != 0u) ? (int)done : FS_ERR_IO;
        }
    }
    if (out_new_size) {
        *out_new_size = cur_size;
    }

    return (int)done;
}

static int ext4_lookup_in_dir_locked(uint32_t dir_ino, const uint8_t *name, uint32_t name_len, uint32_t *out_ino) {
    uint32_t dir_size;
    uint32_t block_count;
    int src;
    if (!name || name_len == 0u || name_len > EXT4_MAX_NAME || !out_ino) {
        return FS_ERR_INVAL;
    }
    if (ext4_read_inode_locked(dir_ino, g_inode_buf) != 0) {
        return FS_ERR_IO;
    }
    if ((ext4_inode_mode(g_inode_buf) & EXT4_S_IFMT) != EXT4_S_IFDIR) {
        return FS_ERR_NOTDIR;
    }
    src = ext4_inode_size32(g_inode_buf, &dir_size);
    if (src != 0) {
        return src;
    }
    block_count = (dir_size + g_ext4.block_size - 1u) / g_ext4.block_size;

    for (uint32_t lblock = 0u; lblock < block_count; lblock++) {
        uint32_t pblock = 0u;
        uint32_t pos = 0u;
        int rc = ext4_inode_map_block_locked(g_inode_buf, lblock, &pblock);
        if (rc == FS_ERR_NOENT) {
            continue;
        }
        if (rc != 0 || ext4_read_block_locked(pblock, g_io_block) != 0) {
            return FS_ERR_IO;
        }
        while (pos + 8u <= g_ext4.block_size) {
            uint32_t ino = rd_le32(&g_io_block[pos + 0u]);
            uint16_t rec_len = rd_le16(&g_io_block[pos + 4u]);
            uint8_t nlen = g_io_block[pos + 6u];
            if (rec_len < 8u || pos + rec_len > g_ext4.block_size) {
                break;
            }
            if (ino != 0u && nlen == name_len && nlen <= (uint8_t)(rec_len - 8u) &&
                mem_eq_u8(&g_io_block[pos + 8u], name, name_len)) {
                *out_ino = ino;
                return 0;
            }
            pos += rec_len;
        }
    }
    return FS_ERR_NOENT;
}

static int ext4_lookup_path_follow_locked(const char *path,
                                          uint32_t *out_ino,
                                          uint16_t *out_mode,
                                          uint32_t *out_size,
                                          uint32_t depth) {
    uint32_t cur_ino = EXT4_ROOT_INO;
    uint32_t i = 0u;
    if (!path || path[0] != '/' || !out_ino || !out_mode || !out_size) {
        return FS_ERR_INVAL;
    }
    if (depth > EXT4_SYMLINK_FOLLOW_MAX) {
        return FS_ERR_LOOP;
    }

    while (path[i] == '/') {
        i++;
    }
    if (path[i] == '\0') {
        int src;
        if (ext4_read_inode_locked(cur_ino, g_inode_buf) != 0) {
            return FS_ERR_IO;
        }
        src = ext4_inode_size32(g_inode_buf, out_size);
        if (src != 0) {
            return src;
        }
        *out_ino = cur_ino;
        *out_mode = ext4_inode_mode(g_inode_buf);
        return 0;
    }

    while (path[i] != '\0') {
        uint8_t comp[EXT4_MAX_NAME];
        uint32_t comp_start;
        uint32_t rem_pos;
        uint32_t n = 0u;
        uint32_t next_ino = 0u;

        while (path[i] == '/') {
            i++;
        }
        comp_start = i;
        while (path[i] != '\0' && path[i] != '/') {
            if (n >= EXT4_MAX_NAME) {
                return FS_ERR_INVAL;
            }
            comp[n++] = (uint8_t)path[i++];
        }
        rem_pos = i;
        if (n == 0u) {
            break;
        }
        {
            int lrc = ext4_lookup_in_dir_locked(cur_ino, comp, n, &next_ino);
            if (lrc != 0) {
                return lrc;
            }
        }
        if (ext4_read_inode_locked(next_ino, g_inode_buf) != 0) {
            return FS_ERR_IO;
        }
        if ((ext4_inode_mode(g_inode_buf) & EXT4_S_IFMT) == EXT4_S_IFLNK) {
            char target[EXT4_PATH_CAP];
            char combined[EXT4_PATH_CAP];
            char normalized[EXT4_PATH_CAP];
            uint32_t target_len = 0u;
            uint32_t combined_len = 0u;
            int rc = ext4_read_symlink_target_locked(next_ino, target, (uint32_t)sizeof(target), &target_len);
            if (rc != 0) {
                return rc;
            }
            if (target_len == 0u) {
                return FS_ERR_NOENT;
            }
            if (target[0] == '/') {
                combined[0] = '\0';
                combined_len = 0u;
            } else {
                rc = ext4_path_copy_prefix(combined, &combined_len, path, comp_start);
                if (rc != 0) {
                    return rc;
                }
                if (combined_len > 1u) {
                    rc = ext4_path_append_char(combined, &combined_len, '/');
                    if (rc != 0) {
                        return rc;
                    }
                }
            }
            for (uint32_t j = 0u; j < target_len; j++) {
                rc = ext4_path_append_char(combined, &combined_len, target[j]);
                if (rc != 0) {
                    return rc;
                }
            }
            while (path[rem_pos] == '/') {
                rem_pos++;
            }
            if (path[rem_pos] != '\0') {
                rc = ext4_path_append_char(combined, &combined_len, '/');
                if (rc != 0) {
                    return rc;
                }
                while (path[rem_pos] != '\0') {
                    rc = ext4_path_append_char(combined, &combined_len, path[rem_pos++]);
                    if (rc != 0) {
                        return rc;
                    }
                }
            }
            rc = ext4_path_normalize_absolute(combined, normalized);
            if (rc != 0) {
                return rc;
            }
            return ext4_lookup_path_follow_locked(normalized, out_ino, out_mode, out_size, depth + 1u);
        }
        cur_ino = next_ino;
        while (path[i] == '/') {
            i++;
        }
    }

    {
        int src;
        if (ext4_read_inode_locked(cur_ino, g_inode_buf) != 0) {
            return FS_ERR_IO;
        }
        src = ext4_inode_size32(g_inode_buf, out_size);
        if (src != 0) {
            return src;
        }
    }
    *out_ino = cur_ino;
    *out_mode = ext4_inode_mode(g_inode_buf);
    return 0;
}

static int ext4_lookup_path_locked(const char *path, uint32_t *out_ino, uint16_t *out_mode, uint32_t *out_size) {
    return ext4_lookup_path_follow_locked(path, out_ino, out_mode, out_size, 0u);
}

static int ext4_try_probe_locked(void) {
    uint16_t magic;
    uint32_t log_block_size;
    uint32_t blocks_count_lo;
    uint32_t blocks_count_hi;
    uint32_t blocks_count32;
    uint32_t groups32;

    if (g_ext4.probe_done != 0u) {
        return g_ext4.mounted ? 0 : FS_ERR_NOENT;
    }

    if (blk_read_sectors(EXT4_FS_LBA_BASE + EXT4_SUPER_LBA_OFF,
                         EXT4_SUPER_LBA_COUNT,
                         (uint32_t)(uintptr_t)&g_super[0]) != BLK_OK) {
        g_ext4.probe_done = 1u;
        g_ext4.mounted = 0u;
        return FS_ERR_NOENT;
    }

    magic = rd_le16(&g_super[0x38u]);
    if (magic != EXT4_SUPER_MAGIC) {
        g_ext4.probe_done = 1u;
        g_ext4.mounted = 0u;
        return FS_ERR_NOENT;
    }

    log_block_size = rd_le32(&g_super[0x18u]);
    if (log_block_size > 2u) {
        return ext4_probe_fail_nosys("log_block_size", log_block_size, 2u);
    }

    g_ext4.block_size = 1024u << log_block_size;
    g_ext4.sectors_per_block = g_ext4.block_size / 512u;
    g_ext4.first_data_block = rd_le32(&g_super[0x14u]);
    g_ext4.blocks_per_group = rd_le32(&g_super[0x20u]);
    g_ext4.inodes_per_group = rd_le32(&g_super[0x28u]);
    g_ext4.inode_size = rd_le16(&g_super[0x58u]);
    g_ext4.feature_incompat = rd_le32(&g_super[0x60u]);
    g_ext4.desc_size = rd_le16(&g_super[0xFEu]);
    if (g_ext4.desc_size < 32u) {
        g_ext4.desc_size = 32u;
    }
    if (g_ext4.desc_size > 64u) {
        return ext4_probe_fail_nosys("desc_size", g_ext4.desc_size, 64u);
    }

    if (g_ext4.inode_size < 128u || g_ext4.inode_size > sizeof(g_inode_buf) ||
        g_ext4.blocks_per_group == 0u || g_ext4.inodes_per_group == 0u ||
        g_ext4.sectors_per_block == 0u) {
        return ext4_probe_fail_nosys("inode/group", g_ext4.inode_size, g_ext4.sectors_per_block);
    }

    blocks_count_lo = rd_le32(&g_super[0x04u]);
    blocks_count_hi = rd_le32(&g_super[0x150u]);
    if (blocks_count_hi != 0u) {
        return ext4_probe_fail_nosys("blocks_hi", blocks_count_hi, blocks_count_lo);
    }
    blocks_count32 = blocks_count_lo;
    if (blocks_count32 <= g_ext4.first_data_block) {
        return ext4_probe_fail_nosys("blocks_count", blocks_count32, g_ext4.first_data_block);
    }
    groups32 = (blocks_count32 - g_ext4.first_data_block + g_ext4.blocks_per_group - 1u) /
               g_ext4.blocks_per_group;
    if (groups32 == 0u) {
        return ext4_probe_fail_nosys("groups", groups32, g_ext4.blocks_per_group);
    }
    g_ext4.blocks_count = blocks_count32;
    g_ext4.group_count = groups32;

    g_ext4.probe_done = 1u;
    g_ext4.mounted = 1u;

    klog_begin(KLOG_LEVEL_INFO, "ext4");
    klog_puts("mounted block=");
    klog_hex32(g_ext4.block_size);
    klog_puts(" groups=");
    klog_hex32(g_ext4.group_count);
    klog_end();
    return 0;
}

void fs_ext4_init(void) {
    g_ext4.probe_done = 0u;
    g_ext4.mounted = 0u;
    g_ext4.block_size = 0u;
    g_ext4.sectors_per_block = 0u;
    g_ext4.blocks_count = 0u;
    g_ext4.first_data_block = 0u;
    g_ext4.blocks_per_group = 0u;
    g_ext4.inodes_per_group = 0u;
    g_ext4.inode_size = 0u;
    g_ext4.group_count = 0u;
    g_ext4.desc_size = 0u;
    g_ext4.feature_incompat = 0u;
    ext4_block_cache_reset();
    spinlock_init(&g_ext4_lock);
    sched_waitq_init(&g_ext4_waitq);
    g_ext4_lock_held = 0u;
}

int fs_ext4_open(const char *path, uint32_t flags) {
    uint32_t ino = 0u;
    uint32_t size = 0u;
    uint16_t mode = 0u;
    uint32_t accmode = flags & SYS_O_ACCMODE;
    uint32_t status_flags = flags & (SYS_O_ACCMODE | SYS_O_NONBLOCK);
    int rc = 0;

    if (!path || path[0] != '/') {
        return FS_ERR_INVAL;
    }
    if ((flags & SYS_O_CREAT) != 0u) {
        return FS_ERR_ROFS;
    }
    if ((flags & SYS_O_TRUNC) != 0u && accmode == SYS_O_RDONLY) {
        return FS_ERR_INVAL;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    if (rc != 0) {
        ext4_mutex_unlock();
        return rc;
    }
    rc = ext4_lookup_path_locked(path, &ino, &mode, &size);
    if (rc == 0 && (mode & EXT4_S_IFMT) == EXT4_S_IFREG &&
        (flags & SYS_O_TRUNC) != 0u && size != 0u) {
        rc = ext4_set_inode_size_locked(ino, 0u);
        if (rc == 0) {
            size = 0u;
        }
    }
    ext4_mutex_unlock();
    if (rc != 0) {
        return rc;
    }
    if ((mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        if (accmode != SYS_O_RDONLY || (flags & SYS_O_TRUNC) != 0u) {
            return FS_ERR_ISDIR;
        }
        return sched_fd_open_regular(status_flags, FS_BACKEND_EXT4, ino, size, 1u);
    }
    if ((mode & EXT4_S_IFMT) != EXT4_S_IFREG) {
        return FS_ERR_INVAL;
    }
    return sched_fd_open_regular(status_flags, FS_BACKEND_EXT4, ino, size, 0u);
}

int fs_ext4_stat(const char *path, fs_stat_t *st) {
    uint32_t ino = 0u;
    uint32_t size = 0u;
    uint16_t mode = 0u;
    uint16_t links = 1u;
    int rc;

    if (!path || path[0] != '/' || !st) {
        return FS_ERR_INVAL;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    if (rc == 0) {
        rc = ext4_lookup_path_locked(path, &ino, &mode, &size);
        if (rc == 0) {
            links = ext4_inode_links_count(g_inode_buf);
        }
    }
    ext4_mutex_unlock();
    if (rc != 0) {
        return rc;
    }

    st->st_dev = FS_BACKEND_EXT4;
    st->st_ino = ino;
    st->st_mode = (uint32_t)mode;
    st->st_nlink = links ? (uint32_t)links : 1u;
    st->st_uid = 0u;
    st->st_gid = 0u;
    st->st_rdev = 0u;
    st->st_size = size;
    st->st_blksize = g_ext4.block_size ? g_ext4.block_size : 4096u;
    st->st_blocks = (size + 511u) / 512u;
    return 0;
}

static uint32_t ext4_dirent_type_to_sys(uint8_t type) {
    switch (type) {
        case 1u:
            return SYS_DT_REG;
        case 2u:
            return SYS_DT_DIR;
        case 3u:
            return SYS_DT_CHR;
        case 7u:
            return SYS_DT_LNK;
        case 6u:
            return SYS_DT_SOCK;
        default:
            return SYS_DT_UNKNOWN;
    }
}

int fs_ext4_getdents_fd(int32_t fd, fs_dirent_t *dst, uint32_t len) {
    uint32_t backend = 0u;
    uint32_t ino = 0u;
    uint32_t size = 0u;
    uint32_t off = 0u;
    uint32_t is_dir = 0u;
    uint32_t written = 0u;
    uint32_t consumed = 0u;
    int rc = 0;

    if (!dst) {
        return FS_ERR_INVAL;
    }
    if (len == 0u) {
        return 0;
    }
    if (len < (uint32_t)sizeof(fs_dirent_t)) {
        return FS_ERR_INVAL;
    }
    if (sched_fd_regular_get(fd, &backend, &ino, &size, &off, &is_dir) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (backend != FS_BACKEND_EXT4) {
        return FS_ERR_BADF;
    }
    if (is_dir == 0u) {
        return FS_ERR_NOTDIR;
    }
    if (off >= size) {
        return 0;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    if (rc == 0) {
        rc = ext4_read_inode_locked(ino, g_inode_buf);
        if (rc == 0 && (ext4_inode_mode(g_inode_buf) & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            rc = FS_ERR_NOTDIR;
        }
    }
    while (rc == 0 && off + consumed < size && written + (uint32_t)sizeof(fs_dirent_t) <= len) {
        uint32_t cur = off + consumed;
        uint32_t logical = cur / g_ext4.block_size;
        uint32_t in_block = cur % g_ext4.block_size;
        uint32_t pblock = 0u;
        uint32_t ino_ent;
        uint16_t rec_len;
        uint8_t nlen;
        uint8_t file_type;
        fs_dirent_t *out = &dst[written / (uint32_t)sizeof(fs_dirent_t)];

        if (in_block + 8u > g_ext4.block_size) {
            consumed += g_ext4.block_size - in_block;
            continue;
        }
        rc = ext4_inode_map_block_locked(g_inode_buf, logical, &pblock);
        if (rc == FS_ERR_NOENT) {
            consumed += g_ext4.block_size - in_block;
            rc = 0;
            continue;
        }
        if (rc != 0 || ext4_read_block_locked(pblock, g_io_block) != 0) {
            rc = FS_ERR_IO;
            break;
        }

        ino_ent = rd_le32(&g_io_block[in_block + 0u]);
        rec_len = rd_le16(&g_io_block[in_block + 4u]);
        nlen = g_io_block[in_block + 6u];
        file_type = g_io_block[in_block + 7u];
        if (rec_len < 8u || in_block + (uint32_t)rec_len > g_ext4.block_size) {
            rc = FS_ERR_IO;
            break;
        }
        consumed += (uint32_t)rec_len;
        if (ino_ent == 0u || nlen == 0u) {
            continue;
        }
        out->d_ino = ino_ent;
        out->d_off = off + consumed;
        out->d_reclen = (uint32_t)sizeof(fs_dirent_t);
        out->d_type = ext4_dirent_type_to_sys(file_type);
        for (uint32_t i = 0u; i < sizeof(out->d_name); i++) {
            out->d_name[i] = '\0';
        }
        for (uint32_t i = 0u; i < (uint32_t)nlen; i++) {
            out->d_name[i] = (char)g_io_block[in_block + 8u + i];
        }
        written += (uint32_t)sizeof(fs_dirent_t);
    }
    ext4_mutex_unlock();
    if (rc != 0) {
        return (written != 0u) ? (int)written : rc;
    }
    if (consumed != 0u) {
        (void)sched_fd_regular_advance(fd, consumed, 0);
    }
    return (int)written;
}

int fs_ext4_readlink(const char *path, uint8_t *dst, uint32_t len) {
    uint32_t parent_ino = EXT4_ROOT_INO;
    uint32_t i = 0u;
    uint32_t link_ino = 0u;
    char target[EXT4_PATH_CAP];
    uint32_t target_len = 0u;
    int rc;

    if (!path || path[0] != '/' || !dst || len == 0u) {
        return FS_ERR_INVAL;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    while (rc == 0 && path[i] == '/') {
        i++;
    }
    if (rc == 0 && path[i] == '\0') {
        rc = FS_ERR_INVAL;
    }
    while (rc == 0 && path[i] != '\0') {
        uint8_t comp[EXT4_MAX_NAME];
        uint32_t comp_start;
        uint32_t n = 0u;
        uint32_t next_ino = 0u;
        while (path[i] == '/') {
            i++;
        }
        comp_start = i;
        while (path[i] != '\0' && path[i] != '/') {
            if (n >= EXT4_MAX_NAME) {
                rc = FS_ERR_INVAL;
                break;
            }
            comp[n++] = (uint8_t)path[i++];
        }
        if (rc != 0 || n == 0u) {
            break;
        }
        rc = ext4_lookup_in_dir_locked(parent_ino, comp, n, &next_ino);
        if (rc != 0) {
            break;
        }
        while (path[i] == '/') {
            i++;
        }
        if (path[i] == '\0') {
            link_ino = next_ino;
            break;
        }
        if (ext4_read_inode_locked(next_ino, g_inode_buf) != 0) {
            rc = FS_ERR_IO;
            break;
        }
        if ((ext4_inode_mode(g_inode_buf) & EXT4_S_IFMT) == EXT4_S_IFLNK) {
            uint32_t size = 0u;
            uint16_t smode = 0u;
            char combined[EXT4_PATH_CAP];
            uint32_t combined_len = 0u;
            char normalized[EXT4_PATH_CAP];
            rc = ext4_read_symlink_target_locked(next_ino, target, (uint32_t)sizeof(target), &target_len);
            if (rc != 0) {
                break;
            }
            if (target[0] == '/') {
                combined[0] = '\0';
                combined_len = 0u;
            } else {
                rc = ext4_path_copy_prefix(combined, &combined_len, path, comp_start);
                if (rc != 0) {
                    break;
                }
                if (combined_len > 1u) {
                    rc = ext4_path_append_char(combined, &combined_len, '/');
                    if (rc != 0) {
                        break;
                    }
                }
            }
            for (uint32_t j = 0u; j < target_len; j++) {
                rc = ext4_path_append_char(combined, &combined_len, target[j]);
                if (rc != 0) {
                    break;
                }
            }
            if (rc != 0) {
                break;
            }
            if (path[i] != '\0') {
                rc = ext4_path_append_char(combined, &combined_len, '/');
                if (rc != 0) {
                    break;
                }
                while (path[i] != '\0') {
                    rc = ext4_path_append_char(combined, &combined_len, path[i++]);
                    if (rc != 0) {
                        break;
                    }
                }
            }
            if (rc != 0) {
                break;
            }
            rc = ext4_path_normalize_absolute(combined, normalized);
            if (rc == 0) {
                rc = ext4_lookup_path_locked(normalized, &link_ino, &smode, &size);
            }
            (void)size;
            (void)smode;
            break;
        }
        parent_ino = next_ino;
    }
    if (rc == 0) {
        rc = ext4_read_symlink_target_locked(link_ino, target, (uint32_t)sizeof(target), &target_len);
    }
    ext4_mutex_unlock();

    if (rc != 0) {
        return rc;
    }
    if (target_len > len) {
        target_len = len;
    }
    for (uint32_t i_copy = 0u; i_copy < target_len; i_copy++) {
        dst[i_copy] = (uint8_t)target[i_copy];
    }
    return (int)target_len;
}

int fs_ext4_read_fd(int32_t fd, uint8_t *dst, uint32_t len) {
    uint32_t backend = 0u;
    uint32_t ino = 0u;
    uint32_t size = 0u;
    uint32_t off = 0u;
    uint32_t is_dir = 0u;
    int rc;

    if (!dst || len == 0u) {
        return 0;
    }
    if (sched_fd_regular_get(fd, &backend, &ino, &size, &off, &is_dir) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (backend != FS_BACKEND_EXT4) {
        return FS_ERR_BADF;
    }
    if (is_dir != 0u) {
        return FS_ERR_ISDIR;
    }
    if (off >= size) {
        return 0;
    }
    if (len > (size - off)) {
        len = size - off;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    if (rc == 0) {
        rc = ext4_read_inode_data_locked(ino, off, dst, len);
    }
    ext4_mutex_unlock();
    if (rc < 0) {
        return rc;
    }
    if (rc > 0) {
        (void)sched_fd_regular_advance(fd, (uint32_t)rc, 0);
    }
    return rc;
}

int fs_ext4_write_fd(int32_t fd, const uint8_t *src, uint32_t len) {
    uint32_t backend = 0u;
    uint32_t ino = 0u;
    uint32_t size = 0u;
    uint32_t off = 0u;
    uint32_t is_dir = 0u;
    uint32_t new_size = 0u;
    int rc;
    if (sched_fd_regular_get(fd, &backend, &ino, &size, &off, &is_dir) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (backend != FS_BACKEND_EXT4) {
        return FS_ERR_BADF;
    }
    if (is_dir != 0u) {
        return FS_ERR_ISDIR;
    }
    if (!src || len == 0u) {
        return 0;
    }

    ext4_mutex_lock();
    rc = ext4_try_probe_locked();
    if (rc == 0) {
        rc = ext4_write_inode_data_locked(ino, off, src, len, &new_size);
    }
    ext4_mutex_unlock();
    if (rc <= 0) {
        return rc;
    }
    (void)sched_fd_regular_commit_write(fd, (uint32_t)rc, new_size, 0);
    return rc;
}
