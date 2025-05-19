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

// BMP file header structure
typedef struct {
    char signature[2];  // Should be "BM"
    u32 fileSize;
    u32 reserved;
    u32 dataOffset;
    u32 headerSize;
    u32 width;
    u32 height;
    u16 planes;
    u16 bitsPerPixel;
    u32 compression;
    u32 imageSize;
    // Rest of the header not needed for this task
} __attribute__((packed)) BMPHeader;

// Directory entry structure for long filenames
struct fat32longent {
    u8 LDIR_Ord;
    u16 LDIR_Name1[5];
    u8 LDIR_Attr;
    u8 LDIR_Type;
    u8 LDIR_Chksum;
    u16 LDIR_Name2[6];
    u16 LDIR_FstClusLO;
    u16 LDIR_Name3[2];
} __attribute__((packed));

// Structure to store information about recovered files
typedef struct {
    char filename[256];
    u32 firstCluster;
    u32 fileSize;
    u8 *data;
    bool recovered;
} RecoveredFile;

// Array to store recovered files
#define MAX_FILES 1024
RecoveredFile recoveredFiles[MAX_FILES];
int numRecoveredFiles = 0;

// Array to track if a cluster has been assigned to a file
bool clusterAssigned[MAX_CLUS] = {false};

// Function to check if a cluster contains BMP header
bool is_bmp_header(void *cluster_data) {
    u8 *data = (u8*)cluster_data;
    return (data[0] == 'B' && data[1] == 'M');
}

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
        if (dent->DIR_Attr == ATTR_VOLUME_ID || (dent->DIR_Attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue;
        char filename[13];
        get_filename(dent, filename);
        u32 firstCluster = (((u32)dent->DIR_FstClusHI) << 16) | dent->DIR_FstClusLO;
        if (firstCluster >= 2 && firstCluster < MAX_CLUS) {
            clus_to_name[firstCluster] = strdup(filename);
        }
    }
}

// Function to scan for directory entries
void scan_for_dir_entries() {
    u32 total_clusters = (hdr->BPB_TotSec32 - (hdr->BPB_RsvdSecCnt + hdr->BPB_NumFATs * hdr->BPB_FATSz32)) / hdr->BPB_SecPerClus;
    
    for (u32 clus = 2; clus < total_clusters + 2; clus++) {
        void *cluster_data = cluster_to_sec(clus);
        if (!cluster_data) continue;
        
        // Check if this looks like a directory cluster (contains directory entries)
        struct fat32dent *dents = (struct fat32dent *)cluster_data;
        int num_entries = (hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec) / sizeof(struct fat32dent);
        
        for (int i = 0; i < num_entries; i++) {
            struct fat32dent *dent = &dents[i];
            
            // If we find a deleted directory entry (.bmp file)
            if (dent->DIR_Name[0] == 0xE5) {
                // Check if it's likely a BMP file
                bool is_likely_bmp = false;
                // Check file extension (BMP)
                if (dent->DIR_Name[8] == 'B' && dent->DIR_Name[9] == 'M' && dent->DIR_Name[10] == 'P') {
                    is_likely_bmp = true;
                }
                
                if (is_likely_bmp) {
                    u32 firstCluster = (((u32)dent->DIR_FstClusHI) << 16) | dent->DIR_FstClusLO;
                    u32 fileSize = dent->DIR_FileSize;
                    
                    // Verify the cluster range is valid
                    if (firstCluster >= 2 && firstCluster < MAX_CLUS) {
                        // Recover original filename
                        char filename[256] = {0};
                        
                        // Handle deleted short name entry
                        char shortName[13] = {0};
                        shortName[0] = '_'; // Replace the first character (which was 0xE5)
                        for (int j = 1; j < 8; j++) {
                            if (dent->DIR_Name[j] == ' ') break;
                            shortName[j] = dent->DIR_Name[j];
                        }
                        int len = strlen(shortName);
                        if (dent->DIR_Name[8] != ' ') {
                            shortName[len++] = '.';
                            for (int j = 8; j < 11; j++) {
                                if (dent->DIR_Name[j] == ' ') break;
                                shortName[len++] = dent->DIR_Name[j];
                            }
                        }
                        shortName[len] = '\0';
                        
                        // Check if we have long filename entries before this entry
                        bool has_long_name = false;
                        char longName[256] = {0};
                        
                        // Look backward for long name entries
                        if (i > 0) {
                            int longIndex = i - 1;
                            struct fat32longent *longDent = (struct fat32longent *)&dents[longIndex];
                            
                            // Check if it's a long name entry (attr == 0x0F)
                            if (longDent->LDIR_Attr == ATTR_LONG_NAME) {
                                has_long_name = true;
                                int nameIndex = 0;
                                
                                // Process the long name entries in reverse order
                                while (longIndex >= 0 && nameIndex < 255) {
                                    longDent = (struct fat32longent *)&dents[longIndex];
                                    if (longDent->LDIR_Attr != ATTR_LONG_NAME) break;
                                    
                                    // Extract unicode characters from the entry
                                    for (int k = 0; k < 5 && nameIndex < 255; k++) {
                                        char c = longDent->LDIR_Name1[k] & 0xFF;
                                        if (c == 0 || c == 0xFF) break;
                                        longName[nameIndex++] = c;
                                    }
                                    
                                    for (int k = 0; k < 6 && nameIndex < 255; k++) {
                                        char c = longDent->LDIR_Name2[k] & 0xFF;
                                        if (c == 0 || c == 0xFF) break;
                                        longName[nameIndex++] = c;
                                    }
                                    
                                    for (int k = 0; k < 2 && nameIndex < 255; k++) {
                                        char c = longDent->LDIR_Name3[k] & 0xFF;
                                        if (c == 0 || c == 0xFF) break;
                                        longName[nameIndex++] = c;
                                    }
                                    
                                    longIndex--;
                                }
                                longName[nameIndex] = '\0';
                                
                                // Reverse the string if needed
                                if (strlen(longName) > 0) {
                                    strcpy(filename, longName);
                                }
                            }
                        }
                        
                        // Use short name if no long name found
                        if (!has_long_name) {
                            strcpy(filename, shortName);
                        }
                        
                        // Ensure filename ends with .bmp
                        if (strlen(filename) < 4 || strcasecmp(filename + strlen(filename) - 4, ".bmp") != 0) {
                            strcat(filename, ".bmp");
                        }
                        
                        // Only add printable ASCII characters to filename
                        for (int j = 0; filename[j] != '\0'; j++) {
                            if (filename[j] < 32 || filename[j] > 126) {
                                filename[j] = '_';
                            }
                        }
                        
                        // Add to recovered files
                        if (numRecoveredFiles < MAX_FILES && fileSize > 0) {
                            strcpy(recoveredFiles[numRecoveredFiles].filename, filename);
                            recoveredFiles[numRecoveredFiles].firstCluster = firstCluster;
                            recoveredFiles[numRecoveredFiles].fileSize = fileSize;
                            recoveredFiles[numRecoveredFiles].data = NULL;
                            recoveredFiles[numRecoveredFiles].recovered = false;
                            numRecoveredFiles++;
                            
                            // Also update cluster->filename mapping
                            if (clus_to_name[firstCluster] == NULL) {
                                clus_to_name[firstCluster] = strdup(filename);
                            }
                        }
                    }
                }
            }
        }
    }
}

// Function to recover a file by its first cluster
void recover_file(RecoveredFile *file) {
    // Verify the first cluster contains a BMP header
    void *first_cluster_data = cluster_to_sec(file->firstCluster);
    if (!first_cluster_data || !is_bmp_header(first_cluster_data)) {
        return;
    }
    
    // Calculate how many clusters we need
    u32 cluster_size = hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec;
    u32 clusters_needed = (file->fileSize + cluster_size - 1) / cluster_size;
    
    // Allocate memory for the file data
    file->data = (u8*)malloc(file->fileSize);
    if (!file->data) {
        return;
    }
    
    // Copy the first cluster
    memcpy(file->data, first_cluster_data, cluster_size < file->fileSize ? cluster_size : file->fileSize);
    clusterAssigned[file->firstCluster] = true;
    
    // Copy remaining clusters (assuming they are contiguous)
    u32 remaining_size = file->fileSize > cluster_size ? file->fileSize - cluster_size : 0;
    u32 current_cluster = file->firstCluster + 1;
    u32 offset = cluster_size;
    
    while (remaining_size > 0 && current_cluster < MAX_CLUS) {
        void *cluster_data = cluster_to_sec(current_cluster);
        if (!cluster_data) break;
        
        u32 copy_size = cluster_size < remaining_size ? cluster_size : remaining_size;
        memcpy(file->data + offset, cluster_data, copy_size);
        clusterAssigned[current_cluster] = true;
        
        remaining_size -= copy_size;
        offset += copy_size;
        current_cluster++;
    }
    
    // Mark as recovered if we got all the data
    if (remaining_size == 0) {
        file->recovered = true;
    } else {
        free(file->data);
        file->data = NULL;
    }
}

// Function to scan for BMP headers directly
void scan_for_bmp_headers() {
    u32 total_clusters = (hdr->BPB_TotSec32 - (hdr->BPB_RsvdSecCnt + hdr->BPB_NumFATs * hdr->BPB_FATSz32)) / hdr->BPB_SecPerClus;
    u32 cluster_size = hdr->BPB_SecPerClus * hdr->BPB_BytsPerSec;
    
    for (u32 clus = 2; clus < total_clusters + 2; clus++) {
        // Skip if already assigned to a file
        if (clusterAssigned[clus]) continue;
        
        void *cluster_data = cluster_to_sec(clus);
        if (!cluster_data) continue;
        
        // Check for BMP header
        if (is_bmp_header(cluster_data)) {
            BMPHeader *bmpHeader = (BMPHeader*)cluster_data;
            u32 fileSize = bmpHeader->fileSize;
            
            // Verify it's a reasonable size and within the filesystem limits
            if (fileSize > 0 && fileSize < 100 * 1024 * 1024) { // Max 100MB to be safe
                char filename[256];
                
                // Generate a filename if we don't have one for this cluster
                if (clus_to_name[clus]) {
                    strcpy(filename, clus_to_name[clus]);
                } else {
                    sprintf(filename, "recovered_bmp_%u.bmp", clus);
                }
                
                // Add to recovered files
                if (numRecoveredFiles < MAX_FILES) {
                    strcpy(recoveredFiles[numRecoveredFiles].filename, filename);
                    recoveredFiles[numRecoveredFiles].firstCluster = clus;
                    recoveredFiles[numRecoveredFiles].fileSize = fileSize;
                    recoveredFiles[numRecoveredFiles].data = NULL;
                    recoveredFiles[numRecoveredFiles].recovered = false;
                    numRecoveredFiles++;
                }
            }
        }
    }
}

// Function to write data to a temporary file and get SHA1
void compute_sha1(RecoveredFile *file) {
    if (!file->recovered || !file->data) return;
    
    // Create a temporary file
    char tmpfilename[128];
    sprintf(tmpfilename, "/tmp/fsrecov_tmp_%u", file->firstCluster);
    FILE *tmp = fopen(tmpfilename, "wb");
    if (!tmp) return;
    
    // Write the data
    fwrite(file->data, 1, file->fileSize, tmp);
    fclose(tmp);
    
    // Run sha1sum
    char command[256];
    sprintf(command, "sha1sum %s", tmpfilename);
    FILE *fp = popen(command, "r");
    if (!fp) {
        unlink(tmpfilename);
        return;
    }
    
    // Read the output
    char sha1[45] = {0}; // SHA1 is 40 chars + space
    if (fscanf(fp, "%s", sha1) == 1) {
        printf("%s  %s\n", sha1, file->filename);
        fflush(stdout); // Flush after each output to ensure visibility even if program times out
    }
    
    pclose(fp);
    unlink(tmpfilename); // Clean up
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

    // 2. Scan for directory entries throughout the filesystem
    scan_for_dir_entries();
    
    // 3. Scan for BMP headers directly (as backup)
    scan_for_bmp_headers();
    
    // 4. Recover and process each file
    for (int i = 0; i < numRecoveredFiles; i++) {
        recover_file(&recoveredFiles[i]);
        
        if (recoveredFiles[i].recovered) {
            compute_sha1(&recoveredFiles[i]);
            // Free memory
            free(recoveredFiles[i].data);
            recoveredFiles[i].data = NULL;
        }
    }
    
    munmap(hdr, hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec);
    // Free allocated names
    for (int i = 0; i < MAX_CLUS; i++) {
        if (clus_to_name[i]) {
            free(clus_to_name[i]);
        }
    }
    return 0;
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
