#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "fat32.h"

void *map_disk(const char *fname);
void *cluster_to_sec(int n);
void get_filename(struct fat32dent *dent, char *buf);
u32 next_cluster(int n);
struct fat32hdr *hdr;

// Map to store cluster -> filename (only from root directory)
#include <stdbool.h>
#define MAX_CLUS 65536
char *clus_to_name[MAX_CLUS];

// Scan root directory only, collect cluster->filename mapping
void scan_root_dir_and_collect_names(u32 clusId) {
    void *cluster_data = cluster_to_sec(clusId);
    if (!cluster_data) return;
    struct fat32dent *dents = (struct fat32dent *)cluster_data;
    int num_entries = (hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec) / sizeof(struct fat32dent);
    for (int i = 0; i < num_entries; i++) {
        struct fat32dent *dent = &dents[i];
        if (dent->DIR_Name[0] == 0) break;
        if (dent->DIR_Name[0] == 0xE5) continue;
        if (dent->DIR_Attr == ATTR_VOLUME_ID || (dent->DIR_Attr & 0x0F) == 0x0F) continue;
        char filename[13];
        get_filename(dent, filename);
        u32 firstCluster = (((u32)dent->DIR_FstClusHI) << 16) | dent->DIR_FstClusLO;
        if (firstCluster >= 2 && firstCluster < MAX_CLUS) {
            clus_to_name[firstCluster] = strdup(filename);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s fs-image\n", argv[0]);
        exit(1);
    }
    setbuf(stdout, NULL);
    assert(sizeof(struct fat32hdr) == 512); // defensive
    hdr = map_disk(argv[1]);

    // 1. Collect root directory file names (no recursion)
    scan_root_dir_and_collect_names(hdr->BPB_RootClus);

    // 2. Linear scan all clusters
    printf("\n--- Performing full cluster scan for recovery ---\n");
    u32 total_clusters = (hdr->BPB_TotSec32 - (hdr->BPB_RsvdSecCnt + hdr->BPB_NumFATs * hdr->BPB_FATSz32)) / hdr->BPB_SecPerClus;
    for (u32 clus = 2; clus < total_clusters + 2; clus++) {
        void *cluster_data = cluster_to_sec(clus);
        if (!cluster_data) continue;
        u8 *data = (u8 *)cluster_data;
        // Print filename if known
        if (clus < MAX_CLUS && clus_to_name[clus]) {
            printf("[Cluster %u] File: %s\n", clus, clus_to_name[clus]);
        } else {
            printf("[Cluster %u]\n", clus);
        }
        // BMP header check
        if (data[0] == 'B' && data[1] == 'M') {
            printf("  BMP Header detected\n");
        } else {
            int is_pixel_data = 1;
            int check_bytes = 100;
            for (int i = 0; i < check_bytes && i < hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec; i++) {
                if (data[i] < 32 && data[i] != 0) {
                    is_pixel_data = 0;
                    break;
                }
            }
            if (is_pixel_data) {
                printf("  Likely BMP pixel data\n");
            } else {
                printf("  Unknown/Unused\n");
            }
        }
    }
    munmap(hdr, hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec);
    // Free allocated names
    for (int i = 0; i < MAX_CLUS; i++) if (clus_to_name[i]) free(clus_to_name[i]);
}

void *map_disk(const char *fname) {
    int fd = open(fname, O_RDWR);

    if (fd < 0) {
        perror(fname);
        goto release;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror(fname);
        goto release;
    }

    struct fat32hdr *hdr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (hdr == (void *)-1) {
        goto release;
    }

    close(fd);

    if (hdr->Signature_word != 0xaa55 ||
            hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec != size) {
        fprintf(stderr, "%s: Not a FAT file image\n", fname);
        goto release;
    }

    printf("%s: DOS/MBR boot sector, ", fname);
    printf("OEM-ID \"%s\", ", hdr->BS_OEMName);
    printf("sectors/cluster %d, ", hdr->BPB_SecPerClus);
    printf("sectors %d, ", hdr->BPB_TotSec32);
    printf("sectors/FAT %d, ", hdr->BPB_FATSz32);
    printf("serial number 0x%x\n", hdr->BS_VolID);

    return hdr;

release:
    if (fd > 0) {
        close(fd);
    }
    exit(1);
}

u32 next_cluster(int n) {
    // RTFM: Sec 4.1

    u32 off = hdr->BPB_RsvdSecCnt * hdr->BPB_BytsPerSec;
    u32 *fat = (u32 *)((u8 *)hdr + off);
    return fat[n];
}

void *cluster_to_sec(int n) {
    // RTFM: Sec 3.5 and 4 (TRICKY)
    // Don't copy code. Write your own.

    u32 DataSec = hdr->BPB_RsvdSecCnt + hdr->BPB_NumFATs * hdr->BPB_FATSz32;
    DataSec += (n - 2) * hdr->BPB_SecPerClus;
    return ((char *)hdr) + DataSec * hdr->BPB_BytsPerSec;
}

void get_filename(struct fat32dent *dent, char *buf) {
    // RTFM: Sec 6.1

    int len = 0;
    for (int i = 0; i < sizeof(dent->DIR_Name); i++) {
        if (dent->DIR_Name[i] != ' ') {
            if (i == 8)
                buf[len++] = '.';
            buf[len++] = dent->DIR_Name[i];
        }
    }
    buf[len] = '\0';
}
