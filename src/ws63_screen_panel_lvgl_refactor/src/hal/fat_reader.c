/**
 * @file fat_reader.c
 * @brief Minimal FAT16/FAT32 root-directory reader for G-code files.
 */
#include "fat_reader.h"
#include "sd_spi.h"

#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

#define FAT_ATTR_LONG_NAME      0x0FU
#define FAT_ATTR_VOLUME_ID      0x08U
#define FAT_ATTR_DIRECTORY      0x10U
#define FAT_ENTRY_DELETED       0xE5U
#define FAT_ENTRY_END           0x00U
#define FAT_SECTOR_SIZE         512U
#define FAT_LFN_CHARS_PER_ENTRY 13U
#define FAT_LFN_MAX_CHARS       (FAT_READER_NAME_MAX - 1U)
#define FAT_MAX_SECTORS_PER_CLUSTER 128U
#define FAT32_RESERVED_CLUSTER  0x0FFFFFF0U

typedef enum {
    FAT_TYPE_NONE = 0,
    FAT_TYPE_16,
    FAT_TYPE_32,
} fat_type_t;

typedef struct {
    bool mounted;
    fat_type_t type;
    uint32_t part_lba;
    uint8_t sectors_per_cluster;
    uint16_t root_entry_count;
    uint32_t fat_start_lba;
    uint32_t fat_size_sectors;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
    uint32_t root_cluster;
    uint32_t data_start_lba;
    uint32_t total_sectors;
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint32_t max_valid_cluster;
    uint64_t volume_end_lba;
} fat_volume_t;

static fat_volume_t g_fat;
static fat_reader_file_t g_files[FAT_READER_MAX_FILES];
static uint8_t g_file_count;
static char g_last_error[48] = "未挂载";
static uint8_t g_sector[FAT_SECTOR_SIZE];

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void set_error(const char *text)
{
    if (text == NULL) {
        text = "未知错误";
    }
    (void)snprintf(g_last_error, sizeof(g_last_error), "%s", text);
}

static bool read_sector(uint32_t lba)
{
    if (g_fat.mounted &&
        ((uint64_t)lba < g_fat.part_lba || (uint64_t)lba >= g_fat.volume_end_lba)) {
        set_error("FAT扇区超出卷范围");
        return false;
    }
    if (sd_spi_read_sector(lba, g_sector, sizeof(g_sector)) != ERRCODE_SUCC) {
        set_error(sd_spi_last_error());
        return false;
    }
    return true;
}

static char ascii_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static bool ext_is_gcode(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    char ext[8] = {0};
    uint8_t i = 0;
    dot++;
    while (*dot != '\0' && i < (uint8_t)(sizeof(ext) - 1U)) {
        ext[i++] = ascii_lower(*dot++);
    }
    return strcmp(ext, "gcode") == 0 ||
           strcmp(ext, "nc") == 0 ||
           strcmp(ext, "gco") == 0;
}

static void short_name_to_text(const uint8_t *entry, char *out, size_t out_size)
{
    char name[9] = {0};
    char ext[4] = {0};
    uint8_t n = 0;
    uint8_t e = 0;

    for (uint8_t i = 0; i < 8U && entry[i] != ' '; i++) {
        name[n++] = (char)entry[i];
    }
    for (uint8_t i = 0; i < 3U && entry[8U + i] != ' '; i++) {
        ext[e++] = (char)entry[8U + i];
    }

    if (e > 0U) {
        (void)snprintf(out, out_size, "%s.%s", name, ext);
    } else {
        (void)snprintf(out, out_size, "%s", name);
    }
}

static void lfn_clear(char *lfn)
{
    memset(lfn, 0, FAT_READER_NAME_MAX);
}

static void lfn_copy_char(char *lfn, uint32_t *pos, uint16_t ch)
{
    if (ch == 0x0000U || ch == 0xFFFFU || *pos >= FAT_LFN_MAX_CHARS) {
        return;
    }
    lfn[*pos] = (ch < 0x80U && ch >= 0x20U) ? (char)ch : '?';
    (*pos)++;
}

static void lfn_parse_entry(const uint8_t *entry, char *lfn)
{
    uint8_t seq = entry[0] & 0x1FU;
    if (seq == 0U) {
        return;
    }
    uint32_t pos = (uint32_t)(seq - 1U) * FAT_LFN_CHARS_PER_ENTRY;
    if (pos >= FAT_LFN_MAX_CHARS) {
        return;
    }

    for (uint8_t i = 0; i < 5U; i++) {
        lfn_copy_char(lfn, &pos, le16(&entry[1U + i * 2U]));
    }
    for (uint8_t i = 0; i < 6U; i++) {
        lfn_copy_char(lfn, &pos, le16(&entry[14U + i * 2U]));
    }
    for (uint8_t i = 0; i < 2U; i++) {
        lfn_copy_char(lfn, &pos, le16(&entry[28U + i * 2U]));
    }
}

static bool parse_bpb_at(uint32_t part_lba, uint32_t container_sectors)
{
    if (!read_sector(part_lba)) {
        return false;
    }
    if (g_sector[510] != 0x55U || g_sector[511] != 0xAAU) {
        set_error("FAT签名无效");
        return false;
    }

    uint16_t bytes_per_sector = le16(&g_sector[11]);
    uint8_t sectors_per_cluster = g_sector[13];
    uint16_t reserved = le16(&g_sector[14]);
    uint8_t fat_count = g_sector[16];
    uint16_t root_entries = le16(&g_sector[17]);
    uint16_t total16 = le16(&g_sector[19]);
    uint32_t total32 = le32(&g_sector[32]);
    uint16_t fat16_size = le16(&g_sector[22]);
    uint32_t fat32_size = le32(&g_sector[36]);

    if (bytes_per_sector != FAT_SECTOR_SIZE || sectors_per_cluster == 0U ||
        sectors_per_cluster > FAT_MAX_SECTORS_PER_CLUSTER ||
        (sectors_per_cluster & (sectors_per_cluster - 1U)) != 0U ||
        reserved == 0U || fat_count == 0U) {
        set_error("FAT BPB参数无效");
        return false;
    }

    uint32_t total_sectors = (total16 != 0U) ? total16 : total32;
    uint32_t fat_size = (fat16_size != 0U) ? fat16_size : fat32_size;
    if (total_sectors == 0U || fat_size == 0U) {
        set_error("FAT容量参数为零");
        return false;
    }
    if (container_sectors != 0U && total_sectors > container_sectors) {
        set_error("FAT卷容量超过分区范围");
        return false;
    }

    uint64_t root_dir_sectors64 =
        ((uint64_t)root_entries * 32U + (FAT_SECTOR_SIZE - 1U)) / FAT_SECTOR_SIZE;
    uint64_t fat_area_sectors = (uint64_t)fat_count * fat_size;
    uint64_t metadata_sectors = (uint64_t)reserved + fat_area_sectors + root_dir_sectors64;
    if (metadata_sectors >= total_sectors) {
        set_error("FAT元数据范围超过卷容量");
        return false;
    }

    uint64_t volume_end_lba = (uint64_t)part_lba + total_sectors;
    uint64_t first_data_lba = (uint64_t)part_lba + metadata_sectors;
    uint64_t root_dir_lba = (uint64_t)part_lba + reserved + fat_area_sectors;
    if (volume_end_lba > (uint64_t)UINT32_MAX + 1U ||
        first_data_lba >= volume_end_lba || root_dir_lba > UINT32_MAX) {
        set_error("FAT LBA范围溢出");
        return false;
    }

    uint32_t data_sectors = total_sectors - (uint32_t)metadata_sectors;
    uint32_t cluster_count = data_sectors / sectors_per_cluster;

    if (cluster_count < 4085U) {
        set_error("FAT12不支持");
        return false;
    }

    fat_type_t type = (cluster_count < 65525U) ? FAT_TYPE_16 : FAT_TYPE_32;
    uint64_t max_valid_cluster64 = (uint64_t)cluster_count + 1U;
    if ((type == FAT_TYPE_16 &&
         (fat16_size == 0U || root_entries == 0U || root_dir_sectors64 == 0U ||
          max_valid_cluster64 >= 0xFFF0U)) ||
        (type == FAT_TYPE_32 &&
         (fat16_size != 0U || root_entries != 0U || root_dir_sectors64 != 0U ||
          max_valid_cluster64 >= FAT32_RESERVED_CLUSTER))) {
        set_error("FAT类型与BPB布局不一致");
        return false;
    }

    uint32_t entry_size = (type == FAT_TYPE_32) ? 4U : 2U;
    uint64_t fat_entry_capacity = ((uint64_t)fat_size * FAT_SECTOR_SIZE) / entry_size;
    if (fat_entry_capacity < (uint64_t)cluster_count + 2U) {
        set_error("FAT表容量不足");
        return false;
    }

    uint32_t root_cluster = (type == FAT_TYPE_32) ?
        (le32(&g_sector[44]) & 0x0FFFFFFFU) : 0U;
    if (type == FAT_TYPE_32 &&
        (root_cluster < 2U || root_cluster > (uint32_t)max_valid_cluster64)) {
        set_error("FAT32根目录簇无效");
        return false;
    }

    memset(&g_fat, 0, sizeof(g_fat));
    g_fat.type = type;
    g_fat.part_lba = part_lba;
    g_fat.sectors_per_cluster = sectors_per_cluster;
    g_fat.root_entry_count = root_entries;
    g_fat.fat_start_lba = part_lba + reserved;
    g_fat.fat_size_sectors = fat_size;
    g_fat.root_dir_lba = (uint32_t)root_dir_lba;
    g_fat.root_dir_sectors = (uint32_t)root_dir_sectors64;
    g_fat.root_cluster = root_cluster;
    g_fat.data_start_lba = (uint32_t)first_data_lba;
    g_fat.total_sectors = total_sectors;
    g_fat.data_sectors = data_sectors;
    g_fat.cluster_count = cluster_count;
    g_fat.max_valid_cluster = (uint32_t)max_valid_cluster64;
    g_fat.volume_end_lba = volume_end_lba;
    g_fat.mounted = true;
    return true;
}

static bool try_mount_volume(void)
{
    if (!read_sector(0U)) {
        return false;
    }

    uint8_t ptype = g_sector[0x1BEU + 4U];
    uint32_t part_lba = le32(&g_sector[0x1BEU + 8U]);
    uint32_t part_sectors = le32(&g_sector[0x1BEU + 12U]);
    if (g_sector[510] == 0x55U && g_sector[511] == 0xAAU &&
        ptype != 0U && part_lba != 0U && part_sectors != 0U &&
        parse_bpb_at(part_lba, part_sectors)) {
        return true;
    }

    return parse_bpb_at(0U, 0U);
}

static bool fat_eoc(uint32_t cluster)
{
    if (g_fat.type == FAT_TYPE_32) {
        return cluster >= 0x0FFFFFF8U;
    }
    return cluster >= 0xFFF8U;
}

static bool fat_cluster_valid(uint32_t cluster)
{
    return g_fat.mounted && cluster >= 2U &&
           cluster <= g_fat.max_valid_cluster;
}

static bool fat_chain_value_valid(uint32_t cluster)
{
    return fat_eoc(cluster) || fat_cluster_valid(cluster);
}

static bool read_fat_next(uint32_t cluster, uint32_t *next)
{
    if (next == NULL || !fat_cluster_valid(cluster)) {
        set_error("FAT链当前簇无效");
        return false;
    }

    uint32_t entry_size = (g_fat.type == FAT_TYPE_32) ? 4U : 2U;
    uint64_t offset = (uint64_t)cluster * entry_size;
    uint64_t fat_bytes = (uint64_t)g_fat.fat_size_sectors * FAT_SECTOR_SIZE;
    if (offset + entry_size > fat_bytes) {
        set_error("FAT链索引超出FAT表");
        return false;
    }

    uint64_t sector64 = (uint64_t)g_fat.fat_start_lba + offset / FAT_SECTOR_SIZE;
    uint32_t pos = (uint32_t)(offset % FAT_SECTOR_SIZE);
    if (sector64 > UINT32_MAX || sector64 >= g_fat.volume_end_lba) {
        set_error("FAT表LBA溢出");
        return false;
    }

    if (!read_sector((uint32_t)sector64)) {
        return false;
    }

    if (g_fat.type == FAT_TYPE_32) {
        if (pos > FAT_SECTOR_SIZE - 4U) {
            return false;
        }
        *next = le32(&g_sector[pos]) & 0x0FFFFFFFU;
    } else {
        if (pos > FAT_SECTOR_SIZE - 2U) {
            return false;
        }
        *next = le16(&g_sector[pos]);
    }
    if (!fat_chain_value_valid(*next)) {
        set_error("FAT链包含空闲或保留簇");
        return false;
    }
    return true;
}

static bool cluster_to_lba(uint32_t cluster, uint32_t *lba)
{
    if (lba == NULL || !fat_cluster_valid(cluster)) {
        set_error("FAT数据簇无效");
        return false;
    }

    uint64_t data_sector = (uint64_t)(cluster - 2U) * g_fat.sectors_per_cluster;
    uint64_t base = (uint64_t)g_fat.data_start_lba + data_sector;
    if (data_sector + g_fat.sectors_per_cluster > g_fat.data_sectors ||
        base > UINT32_MAX || base + g_fat.sectors_per_cluster > g_fat.volume_end_lba) {
        set_error("FAT数据簇LBA越界");
        return false;
    }
    *lba = (uint32_t)base;
    return true;
}

typedef struct {
    uint32_t anchor;
    uint32_t power;
    uint32_t length;
    uint32_t traversed;
} fat_chain_guard_t;

static void fat_chain_guard_init(fat_chain_guard_t *guard, uint32_t first_cluster)
{
    guard->anchor = first_cluster;
    guard->power = 1U;
    guard->length = 0U;
    guard->traversed = 1U;
}

static bool fat_chain_guard_advance(fat_chain_guard_t *guard, uint32_t next_cluster)
{
    if (guard->traversed >= g_fat.cluster_count) {
        set_error("FAT文件链超过卷簇数");
        return false;
    }
    guard->traversed++;
    guard->length++;
    if (next_cluster == guard->anchor) {
        set_error("FAT文件链循环");
        return false;
    }
    if (guard->length == guard->power) {
        guard->anchor = next_cluster;
        guard->length = 0U;
        guard->power = (guard->power <= g_fat.cluster_count / 2U) ?
            guard->power * 2U : g_fat.cluster_count;
    }
    return true;
}

static bool add_file_entry(const char *name, uint32_t cluster, uint32_t size)
{
    if (g_file_count >= FAT_READER_MAX_FILES || name == NULL || !ext_is_gcode(name)) {
        return false;
    }
    if (size > 0U && !fat_cluster_valid(cluster)) {
        osal_printk("[FAT] skip invalid file cluster name=%s cluster=%lu size=%lu\r\n",
                    name, (unsigned long)cluster, (unsigned long)size);
        return false;
    }
    uint32_t cluster_size = (uint32_t)g_fat.sectors_per_cluster * FAT_SECTOR_SIZE;
    uint64_t required_clusters = ((uint64_t)size + cluster_size - 1U) / cluster_size;
    if (required_clusters > g_fat.cluster_count) {
        osal_printk("[FAT] skip oversized file name=%s size=%lu clusters=%lu\r\n",
                    name, (unsigned long)size, (unsigned long)required_clusters);
        return false;
    }
    fat_reader_file_t *dst = &g_files[g_file_count++];
    (void)snprintf(dst->name, sizeof(dst->name), "%s", name);
    dst->size_bytes = size;
    dst->first_cluster = cluster;
    return true;
}

static bool parse_dir_sector(const uint8_t *sector, bool *end)
{
    char lfn[FAT_READER_NAME_MAX];
    lfn_clear(lfn);

    for (uint32_t off = 0; off < FAT_SECTOR_SIZE; off += 32U) {
        const uint8_t *entry = &sector[off];
        if (entry[0] == FAT_ENTRY_END) {
            *end = true;
            return true;
        }
        if (entry[0] == FAT_ENTRY_DELETED) {
            lfn_clear(lfn);
            continue;
        }

        uint8_t attr = entry[11];
        if (attr == FAT_ATTR_LONG_NAME) {
            lfn_parse_entry(entry, lfn);
            continue;
        }

        if ((attr & FAT_ATTR_VOLUME_ID) != 0U || (attr & FAT_ATTR_DIRECTORY) != 0U) {
            lfn_clear(lfn);
            continue;
        }

        char short_name[16] = {0};
        char final_name[FAT_READER_NAME_MAX] = {0};
        short_name_to_text(entry, short_name, sizeof(short_name));
        (void)snprintf(final_name, sizeof(final_name), "%s", lfn[0] != '\0' ? lfn : short_name);

        uint32_t high = (g_fat.type == FAT_TYPE_32) ? le16(&entry[20]) : 0U;
        uint32_t low = le16(&entry[26]);
        uint32_t cluster = (high << 16) | low;
        if (g_fat.type == FAT_TYPE_32) {
            cluster &= 0x0FFFFFFFU;
        }
        uint32_t size = le32(&entry[28]);
        (void)add_file_entry(final_name, cluster, size);
        lfn_clear(lfn);
    }
    return true;
}

static bool scan_fat16_root(void)
{
    bool end = false;
    for (uint32_t i = 0; i < g_fat.root_dir_sectors && !end; i++) {
        uint64_t lba = (uint64_t)g_fat.root_dir_lba + i;
        if (lba > UINT32_MAX || lba >= g_fat.volume_end_lba ||
            !read_sector((uint32_t)lba)) {
            return false;
        }
        if (!parse_dir_sector(g_sector, &end)) {
            return false;
        }
    }
    return true;
}

static bool scan_fat32_root(void)
{
    uint32_t cluster = g_fat.root_cluster;
    uint32_t traversed = 0U;
    uint32_t cycle_anchor = cluster;
    uint32_t cycle_power = 1U;
    uint32_t cycle_length = 0U;
    bool end = false;

    while (!end) {
        if (!fat_cluster_valid(cluster) || traversed >= g_fat.cluster_count) {
            set_error("FAT32目录链循环或过长");
            return false;
        }
        traversed++;

        uint32_t base = 0U;
        if (!cluster_to_lba(cluster, &base)) {
            return false;
        }
        for (uint8_t s = 0; s < g_fat.sectors_per_cluster && !end; s++) {
            uint64_t lba = (uint64_t)base + s;
            if (lba > UINT32_MAX || lba >= g_fat.volume_end_lba ||
                !read_sector((uint32_t)lba)) {
                return false;
            }
            if (!parse_dir_sector(g_sector, &end)) {
                return false;
            }
        }
        if (end) {
            return true;
        }

        uint32_t current = cluster;
        if (!read_fat_next(current, &cluster)) {
            return false;
        }
        if (fat_eoc(cluster)) {
            return true;
        }
        if (cluster == current) {
            set_error("FAT32目录链自循环");
            return false;
        }

        /* Brent cycle detection is O(1) memory and avoids a DTCM bitmap. */
        cycle_length++;
        if (cluster == cycle_anchor) {
            set_error("FAT32目录链循环");
            return false;
        }
        if (cycle_length == cycle_power) {
            cycle_anchor = cluster;
            cycle_length = 0U;
            cycle_power = (cycle_power <= g_fat.cluster_count / 2U) ?
                cycle_power * 2U : g_fat.cluster_count;
        }
    }
    return true;
}

errcode_t fat_reader_mount(void)
{
    memset(&g_fat, 0, sizeof(g_fat));
    memset(g_files, 0, sizeof(g_files));
    g_file_count = 0;

    if (sd_spi_init_card() != ERRCODE_SUCC) {
        set_error(sd_spi_last_error());
        return ERRCODE_FAIL;
    }
    if (!try_mount_volume()) {
        return ERRCODE_FAIL;
    }

    bool ok = (g_fat.type == FAT_TYPE_32) ? scan_fat32_root() : scan_fat16_root();
    if (!ok) {
        g_fat.mounted = false;
        memset(g_files, 0, sizeof(g_files));
        g_file_count = 0U;
        return ERRCODE_FAIL;
    }

    set_error(g_file_count == 0U ? "SD卡无G-code文件" : "无");
    osal_printk("[FAT] mounted type=FAT%u files=%u spc=%u total=%lu data=%lu clusters=%lu\r\n",
                g_fat.type == FAT_TYPE_32 ? 32U : 16U,
                (unsigned int)g_file_count,
                (unsigned int)g_fat.sectors_per_cluster,
                (unsigned long)g_fat.total_sectors,
                (unsigned long)g_fat.data_start_lba,
                (unsigned long)g_fat.cluster_count);
    return ERRCODE_SUCC;
}

uint8_t fat_reader_file_count(void)
{
    return g_file_count;
}

const fat_reader_file_t *fat_reader_get_file(uint8_t index)
{
    if (index >= g_file_count) {
        return NULL;
    }
    return &g_files[index];
}

errcode_t fat_reader_read_file(uint8_t index, uint32_t offset, uint8_t *out,
                               size_t out_size, size_t *bytes_read, bool *eof)
{
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    if (eof != NULL) {
        *eof = true;
    }
    if (!g_fat.mounted || index >= g_file_count || out == NULL || out_size == 0U) {
        set_error("文件读取参数无效");
        return ERRCODE_FAIL;
    }

    const fat_reader_file_t *file = &g_files[index];
    if (offset >= file->size_bytes) {
        return ERRCODE_SUCC;
    }

    uint32_t cluster_size = (uint32_t)g_fat.sectors_per_cluster * FAT_SECTOR_SIZE;
    uint32_t cluster = file->first_cluster;
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t in_cluster = offset % cluster_size;
    if (!fat_cluster_valid(cluster) || skip_clusters >= g_fat.cluster_count) {
        set_error("文件起始簇或偏移无效");
        return ERRCODE_FAIL;
    }

    fat_chain_guard_t guard;
    fat_chain_guard_init(&guard, cluster);

    for (uint32_t i = 0; i < skip_clusters; i++) {
        uint32_t next = 0U;
        if (!read_fat_next(cluster, &next) || fat_eoc(next)) {
            set_error("FAT链过早结束");
            return ERRCODE_FAIL;
        }
        if (!fat_chain_guard_advance(&guard, next)) {
            return ERRCODE_FAIL;
        }
        cluster = next;
    }

    uint32_t remaining_file = file->size_bytes - offset;
    size_t remaining_out = out_size;
    size_t total = 0;

    while (remaining_file > 0U && remaining_out > 0U) {
        uint32_t base = 0U;
        if (!cluster_to_lba(cluster, &base)) {
            return ERRCODE_FAIL;
        }
        uint32_t sector_index = in_cluster / FAT_SECTOR_SIZE;
        uint32_t sector_offset = in_cluster % FAT_SECTOR_SIZE;

        while (sector_index < g_fat.sectors_per_cluster && remaining_file > 0U && remaining_out > 0U) {
            uint64_t lba = (uint64_t)base + sector_index;
            if (lba > UINT32_MAX || lba >= g_fat.volume_end_lba ||
                !read_sector((uint32_t)lba)) {
                return ERRCODE_FAIL;
            }
            uint32_t avail = FAT_SECTOR_SIZE - sector_offset;
            if (avail > remaining_file) {
                avail = remaining_file;
            }
            if ((size_t)avail > remaining_out) {
                avail = (uint32_t)remaining_out;
            }
            memcpy(&out[total], &g_sector[sector_offset], avail);
            total += avail;
            remaining_out -= avail;
            remaining_file -= avail;
            sector_index++;
            sector_offset = 0U;
        }

        in_cluster = 0U;
        if (remaining_file > 0U && remaining_out > 0U) {
            uint32_t next = 0U;
            if (!read_fat_next(cluster, &next)) {
                return ERRCODE_FAIL;
            }
            if (fat_eoc(next)) {
                set_error("FAT链早于文件大小结束");
                return ERRCODE_FAIL;
            }
            if (!fat_chain_guard_advance(&guard, next)) {
                return ERRCODE_FAIL;
            }
            cluster = next;
        }
    }

    if (bytes_read != NULL) {
        *bytes_read = total;
    }
    if (eof != NULL) {
        *eof = (uint64_t)offset + total >= file->size_bytes;
    }
    return total > 0U ? ERRCODE_SUCC : ERRCODE_FAIL;
}

const char *fat_reader_last_error(void)
{
    return g_last_error;
}
