/**
 * FAT32 File System Recovery Utility
 *
 * This program scans a FAT32 file system image and recovers BMP files
 * that may have been deleted or corrupted.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <glob.h>

#include "fat32.h"

#define MAX_FILE 256
#define OUTPUT_DIR "recovered_bmp_files"

/* Global variables */
static struct fat32hdr *hdr = NULL;
static struct bmp_file *bmp_files = NULL;
static int bmp_count = 0;

/**
 * Long File Name entry structure as per FAT32 specification
 */
struct lfn_entry
{
    u8 LDIR_Ord;        // 0x00: Long name sequence number (with 0x40 for last entry)
    u16 LDIR_Name1[5];  // 0x01: First 5 characters of long name (UCS-2/UTF-16)
    u8 LDIR_Attr;       // 0x0B: Attributes (always 0x0F for LFN)
    u8 LDIR_Type;       // 0x0C: Reserved (always 0)
    u8 LDIR_Chksum;     // 0x0D: Checksum
    u16 LDIR_Name2[6];  // 0x0E: Next 6 characters
    u16 LDIR_FstClusLO; // 0x1A: Reserved (always 0)
    u16 LDIR_Name3[2];  // 0x1C: Last 2 characters
} __attribute__((packed));

/**
 * BMP file metadata structure for recovery
 */
struct bmp_file
{
    char *short_name;       // Short file name (8.3 format)
    char *full_name;        // Long file name if available
    uint32_t first_cluster; // Starting cluster number
    uint32_t size;          // File size in bytes
};

/* Function prototypes */
static void *map_disk(const char *fname);
static void *cluster_to_sec(uint32_t cluster_num);
static void *read_all_dents(void);
static void *read_bmp_data(uint32_t first_cluster, uint32_t size);
static void cleanup_resources(void);

/**
 * Main entry point for the FAT32 file system recovery utility
 *
 * @param argc Number of command-line arguments
 * @param argv Array of command-line argument strings
 * @return Exit status code
 */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s fs-image\n", argv[0]);
        return EXIT_FAILURE;
    }

    setbuf(stdout, NULL);

    /* Verify FAT32 header size */
    assert(sizeof(struct fat32hdr) == 512);

    /* Map disk image to memory */
    hdr = map_disk(argv[1]);
    if (!hdr)
    {
        fprintf(stderr, "Failed to map disk image\n");
        return EXIT_FAILURE;
    }

    /* Allocate memory for BMP file metadata */
    bmp_files = malloc(MAX_FILE * sizeof(struct bmp_file));
    if (bmp_files == NULL)
    {
        perror("Failed to allocate memory for BMP files");
        cleanup_resources();
        return EXIT_FAILURE;
    }
    memset(bmp_files, 0, MAX_FILE * sizeof(struct bmp_file));

    /* Scan and recover BMP files */
    bmp_files = read_all_dents();

    for (size_t i = 0; i < bmp_count; i++)
    {
        struct bmp_file *bmp = &bmp_files[i];

        /* Read BMP data from disk */
        void *bmp_data = read_bmp_data(bmp->first_cluster, bmp->size);

        // Check output directory
        if (access(OUTPUT_DIR, F_OK) == -1)
        {
            if (mkdir(OUTPUT_DIR, 0755) == -1)
            {
                perror("mkdir");
                return -1;
            }
        }

        if (bmp_data)
        {
            /* Save the BMP data to a file */
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s/%s", OUTPUT_DIR, bmp->full_name ? bmp->full_name : bmp->short_name);
            FILE *fp = fopen(full_path, "wb");
            if (fp)
            {
                fwrite(bmp_data, 1, bmp->size, fp);
                fclose(fp);
            }
            else
            {
                perror("Failed to open output file");
            }
            free(bmp_data);
        }
        else
        {
            fprintf(stderr, "[ERROR] Failed to read BMP data for %s\n", bmp->short_name);
        }
    }

    /* Clean up and exit */
    cleanup_resources();

    // Print sha1sum using a pipe, parent prints output
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    pid_t pid1 = fork();
    if (pid1 == 0)
    {
        // Child: sha1sum, write to pipe
        close(pipefd[0]); // Close unused read end
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        glob_t glob_result;
        int ret = glob("recovered_bmp_files/*.bmp", GLOB_BRACE | GLOB_TILDE, NULL, &glob_result);
        if (ret != 0)
        {
            fprintf(stderr, "No matching files found.\n");
            exit(1);
        }
        char **args = malloc((glob_result.gl_pathc + 2) * sizeof(char *));
        args[0] = "sha1sum";
        for (size_t i = 0; i < glob_result.gl_pathc; i++)
        {
            args[i + 1] = glob_result.gl_pathv[i];
        }
        args[glob_result.gl_pathc + 1] = NULL;
        execvp("sha1sum", args);
        perror("execvp");
        globfree(&glob_result);
        free(args);
        exit(1);
    }
    else if (pid1 < 0)
    {
        perror("fork");
        return EXIT_FAILURE;
    }

    // Parent: read from pipe and print to stdout
    close(pipefd[1]); // Close unused write end
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    close(pipefd[0]);
    waitpid(pid1, NULL, 0);

    return EXIT_SUCCESS;
}

/**
 * Map disk image file to memory
 *
 * @param fname File name of the FAT32 image
 * @return Pointer to the memory-mapped FAT32 header or NULL on failure
 */
static void *map_disk(const char *fname)
{
    int fd = -1;
    struct fat32hdr *header = NULL;
    off_t size;

    /* Open the disk image file */
    fd = open(fname, O_RDWR);
    if (fd < 0)
    {
        perror(fname);
        return NULL;
    }

    /* Get file size */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1)
    {
        perror(fname);
        close(fd);
        return NULL;
    }

    /* Memory map the file */
    header = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (header == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return NULL;
    }

    /* File descriptor is no longer needed after mapping */
    close(fd);

    /* Validate that this is a FAT32 file system */
    if (header->Signature_word != 0xaa55 ||
        header->BPB_TotSec32 * header->BPB_BytsPerSec != size)
    {
        fprintf(stderr, "%s: Not a valid FAT32 file image\n", fname);
        munmap(header, size);
        return NULL;
    }

    return header;
}

/**
 * Convert cluster number to sector address
 *
 * @param cluster_num Cluster number (2-based)
 * @return Pointer to the beginning of the cluster data
 */
static void *cluster_to_sec(uint32_t cluster_num)
{
    /* Calculate data area sector */
    u32 data_sector_offset = hdr->BPB_RsvdSecCnt + hdr->BPB_NumFATs * hdr->BPB_FATSz32;

    /* Add offset for the requested cluster */
    // Cluster numbers are 2-based. The first data cluster is cluster 2.
    u32 cluster_offset_in_sectors = (cluster_num - 2) * hdr->BPB_SecPerClus;

    u32 target_sector = data_sector_offset + cluster_offset_in_sectors;

    void *ptr = ((char *)hdr) + target_sector * hdr->BPB_BytsPerSec;

    // Boundary check: Ensure the calculated pointer is within the mapped image bounds
    off_t image_size = hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec;
    void *image_end = (char *)hdr + image_size;

    if (ptr < (void *)hdr || ptr >= image_end)
    {
        fprintf(stderr, "[ERROR] cluster_to_sec: Calculated pointer %p is out of bounds for cluster %u. Image start: %p, Image end: %p\n", ptr, cluster_num, hdr, image_end);
        return NULL;
    }
    if ((char *)ptr + (hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec) > (char *)image_end)
    {
        fprintf(stderr, "[ERROR] cluster_to_sec: Calculated cluster data for cluster %u would read past end of image. Pointer: %p, Cluster size: %u, Image end: %p\n", cluster_num, ptr, hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec, image_end);
        // This might still be an issue if the last cluster is partial, but memcpy in read_bmp_data should handle it.
        // However, if the start of the cluster is already too close to the end, it's problematic.
    }

    /* Return pointer to the data area */
    return ptr;
}

/**
 * Free allocated resources and clean up
 */
static void cleanup_resources(void)
{
    /* Free BMP file metadata structures */
    if (bmp_files)
    {
        for (int i = 0; i < bmp_count; i++)
        {
            free(bmp_files[i].short_name);
            free(bmp_files[i].full_name);
        }
        free(bmp_files);
        bmp_files = NULL;
    }

    /* Unmap disk image */
    if (hdr)
    {
        munmap(hdr, hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec);
        hdr = NULL;
    }
}

/**
 * Scan the FAT32 file system for BMP files
 *
 * This function looks through the entire data area for potential
 * directory entries that could point to BMP files, including those
 * that might have been deleted or partially overwritten.
 */
static void *read_all_dents(void)
{
    u32 data_start_sector;
    u32 total_sectors;
    void *data_area, *data_end;
    const int step_size = 16; /* Scan step size */
    char *p, *p1;

    /* Calculate the start of the data area */
    data_start_sector = hdr->BPB_RsvdSecCnt + (hdr->BPB_NumFATs * hdr->BPB_FATSz32);
    data_area = ((char *)hdr) + data_start_sector * hdr->BPB_BytsPerSec;

    /* Calculate the end of the data area */
    total_sectors = hdr->BPB_TotSec32;
    data_end = ((char *)hdr) + total_sectors * hdr->BPB_BytsPerSec;

    printf("Scanning data area for BMP files...\n");

    /* Iterate through the data area, 16 bytes at a time */
    for (p = (char *)data_area; p < (char *)data_end; p += step_size)
    {
        struct fat32dent *dent = (struct fat32dent *)p;

        /* Check for potential BMP file extension - should be "BMP " at offset 8 */
        if (dent->DIR_Name[8] == 'B' && dent->DIR_Name[9] == 'M' &&
            dent->DIR_Name[10] == 'P' && dent->DIR_Name[11] == 0x20)
        {

            /* Ensure we have space for another BMP file */
            if (bmp_count >= MAX_FILE)
            {
                fprintf(stderr, "Warning: Maximum file count reached, skipping additional files\n");
                break;
            }

            /* Extract file information */
            uint32_t first_cluster = ((uint32_t)dent->DIR_FstClusHI << 16) | dent->DIR_FstClusLO;
            uint32_t size = dent->DIR_FileSize;
            /* Validate cluster range to avoid out-of-bounds access */
            {
                // data_start_sector is already calculated in read_all_dents
                uint32_t num_data_sectors = hdr->BPB_TotSec32 - data_start_sector;
                uint32_t total_data_clusters = num_data_sectors / hdr->BPB_SecPerClus;
                // Cluster IDs are 2-indexed, so the highest valid ID is total_data_clusters + 1.
                uint32_t max_valid_cluster_num = total_data_clusters + 1;

                if (first_cluster < 2 || first_cluster > max_valid_cluster_num)
                {
                    continue;
                }
            }

            /* Process Long File Name entries if present */
            char full_name[64] = {0};
            int full_name_pos = 0;

            /* Look backward for LFN entries */
            p1 = p;
            do
            {
                p1 -= step_size * 2;

                /* Break if we've gone too far back */
                if (p1 <= (char *)data_area)
                {
                    break;
                }

                struct fat32dent *prev_dent = (struct fat32dent *)p1;

                /* Check if it's an LFN entry */
                if (prev_dent->DIR_Attr == ATTR_LONG_NAME)
                {
                    struct lfn_entry *lfn = (struct lfn_entry *)p1;

                    /* Extract characters from LFN entry */
                    for (int i = 0; i < 5 && full_name_pos < 63; i++)
                    {
                        u16 wc = lfn->LDIR_Name1[i];
                        if (wc != 0x0000 && wc != 0xFFFF)
                        {
                            full_name[full_name_pos++] = (char)(wc & 0xFF);
                        }
                    }

                    for (int i = 0; i < 6 && full_name_pos < 63; i++)
                    {
                        u16 wc = lfn->LDIR_Name2[i];
                        if (wc != 0x0000 && wc != 0xFFFF)
                        {
                            full_name[full_name_pos++] = (char)(wc & 0xFF);
                        }
                    }

                    for (int i = 0; i < 2 && full_name_pos < 63; i++)
                    {
                        u16 wc = lfn->LDIR_Name3[i];
                        if (wc != 0x0000 && wc != 0xFFFF)
                        {
                            full_name[full_name_pos++] = (char)(wc & 0xFF);
                        }
                    }
                }
            } while (p1 > (char *)data_area &&
                     ((struct fat32dent *)p1)->DIR_Attr == ATTR_LONG_NAME);

            /* Create short name (8.3 format) */
            char short_name[13] = {0};
            for (int i = 0; i < 11; i++)
            {
                short_name[i] = dent->DIR_Name[i] >= 32 && dent->DIR_Name[i] <= 126 ? dent->DIR_Name[i] : '?';
            }
            short_name[11] = '\0';

            /* Store file information in our array */
            bmp_files[bmp_count].short_name = strdup(short_name);
            bmp_files[bmp_count].first_cluster = first_cluster;
            bmp_files[bmp_count].size = size;

            if (full_name_pos > 0)
            {
                bmp_files[bmp_count].full_name = strdup(full_name);
            }
            else
            {
                bmp_files[bmp_count].full_name = NULL;
            }

            /* Check allocations */
            if (!bmp_files[bmp_count].short_name || (full_name_pos > 0 && !bmp_files[bmp_count].full_name))
            {
                perror("Memory allocation failed for file metadata");
                // Clean up partially allocated memory if full_name was attempted
                if (full_name_pos > 0 && !bmp_files[bmp_count].full_name && bmp_files[bmp_count].short_name)
                {
                    free(bmp_files[bmp_count].short_name);
                    bmp_files[bmp_count].short_name = NULL;
                }
                continue;
            }

            /* Increment the count of found BMP files */
            bmp_count++;
        }
    }

    printf("Scanning complete. Found %d BMP files.\n", bmp_count);

    return bmp_files;
}

/**
 * Read BMP file data from disk clusters
 *
 * @param first_cluster First cluster number of the BMP file
 * @param size Size of the BMP file in bytes
 * @return Pointer to the read BMP data or NULL on failure
 */
static void *read_bmp_data(uint32_t first_cluster, uint32_t size)
{
    uint32_t bytes_per_cluster;
    uint32_t clusters_needed;
    uint32_t bytes_copied = 0;
    uint32_t current_cluster;
    uint32_t bytes_to_copy;
    void *bmp_data;
    void *cluster_data;

    /* Calculate cluster size in bytes */
    bytes_per_cluster = hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec;

    /* Allocate memory for the entire BMP file */
    if (size == 0)
    {
        fprintf(stderr, "[ERROR] read_bmp_data: File size is 0 for first_cluster=%u. Skipping.\n", first_cluster);
        return NULL;
    }
    bmp_data = malloc(size);
    if (bmp_data == NULL)
    {
        perror("Failed to allocate memory for BMP data");
        return NULL;
    }

    /* Calculate number of clusters needed */
    clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    current_cluster = first_cluster;

    /* Copy data cluster by cluster */
    for (uint32_t i = 0; i < clusters_needed; i++)
    {
        /* Get the sector for the current cluster */
        cluster_data = cluster_to_sec(current_cluster);
        if (!cluster_data)
        {
            // Error handling: if cluster_to_sec fails, just return file so far
            fprintf(stderr, "[ERROR] read_bmp_data: cluster_to_sec returned NULL for cluster %u, aborting further reads.\n", current_cluster);
            fprintf(stderr, "[ERROR] read_bmp_data: i=%u, clusters_needed=%u, bytes_copied=%u, file_size=%u\n", i, clusters_needed, bytes_copied, size);
            fprintf(stderr, "[ERROR] Error accessing cluster %u in read_bmp_data\n", current_cluster);
            return bmp_data;
        }

        /* Calculate how many bytes to copy from this cluster */
        bytes_to_copy = (size - bytes_copied < bytes_per_cluster) ? (size - bytes_copied) : bytes_per_cluster;
        /* Copy the data from this cluster to our buffer */
        memcpy((char *)bmp_data + bytes_copied, cluster_data, bytes_to_copy);
        bytes_copied += bytes_to_copy;

        /* Move to the next cluster (assuming sequential clusters) */
        /* This is a simplification - should check FAT for the next cluster */
        current_cluster++;
    }

    return bmp_data;
}