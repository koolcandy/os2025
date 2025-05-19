#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <ctype.h>
#include <strings.h> // For strcasecmp
#include <sys/wait.h> // For fork and wait
#include "fat32.h"

void *map_disk(const char *fname);

// BMP file header structure
typedef struct {
    uint16_t signature;        // "BM" (0x4D42)
    uint32_t fileSize;         // File size in bytes
    uint32_t reserved;         // Reserved
    uint32_t dataOffset;       // Offset to bitmap data
} __attribute__((packed)) BMPHeader;

// BMP info header structure
typedef struct {
    uint32_t headerSize;       // Info header size
    int32_t  width;            // Width of the image
    int32_t  height;           // Height of the image
    uint16_t planes;           // Number of color planes
    uint16_t bitsPerPixel;     // Bits per pixel
    uint32_t compression;      // Compression type
    uint32_t imageSize;        // Image size in bytes
    int32_t  xPixelsPerMeter;  // Horizontal resolution
    int32_t  yPixelsPerMeter;  // Vertical resolution
    uint32_t colorsUsed;       // Number of colors used
    uint32_t colorsImportant;  // Number of important colors
} __attribute__((packed)) BMPInfoHeader;

// Long filename directory entry structure
struct fat32_lfn_entry {
    u8  LDIR_Ord;              // Order of entry in sequence
    u16 LDIR_Name1[5];         // Characters 1-5 of LFN
    u8  LDIR_Attr;             // Attributes - always 0x0F
    u8  LDIR_Type;             // Type of entry - should be 0
    u8  LDIR_Chksum;           // Checksum of short filename
    u16 LDIR_Name2[6];         // Characters 6-11 of LFN
    u16 LDIR_FstClusLO;        // First cluster - must be 0
    u16 LDIR_Name3[2];         // Characters 12-13 of LFN
} __attribute__((packed));

// Constants for FAT32 file system
#define BMP_SIGNATURE 0x4D42   // "BM" in little endian
#define DELETED_FLAG 0xE5      // First byte of deleted file name
#define LFN_LAST_ENTRY 0x40    // Flag for last entry in LFN sequence

// Cluster categories
enum ClusterType {
    CLUSTER_UNUSED,
    CLUSTER_DIR_ENTRY,
    CLUSTER_BMP_HEADER,
    CLUSTER_BMP_DATA
};

// Recovered file information
typedef struct {
    char filename[256];
    uint32_t startCluster;
    uint32_t fileSize;
    bool valid;
    uint32_t *clusters;
    uint32_t clusterCount;
} RecoveredFile;

// Function prototypes
int is_bmp_header(const void *data);
bool is_valid_filename_char(unsigned char c);
int is_directory_cluster(const void *data, size_t size);
char *extract_long_filename(const struct fat32_lfn_entry *lfn_entries, int count);
void recover_file_data(uint32_t startCluster, uint32_t fileSize, uint8_t *disk, 
                      uint32_t clusterSize, uint32_t dataStartSector, uint32_t bytesPerSector,
                      RecoveredFile *file);
void calculate_sha1(RecoveredFile *file, uint8_t *disk, uint32_t clusterSize,
                   uint32_t dataStartSector, uint32_t bytesPerSector);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s fs-image\n", argv[0]);
        exit(1);
    }

    setbuf(stdout, NULL);

    assert(sizeof(struct fat32hdr) == 512); // defensive

    // map disk image to memory
    struct fat32hdr *hdr = map_disk(argv[1]);

    // Calculate FAT32 file system parameters
    uint32_t bytesPerSector = hdr->BPB_BytsPerSec;
    uint32_t sectorsPerCluster = hdr->BPB_SecPerClus;
    uint32_t clusterSize = bytesPerSector * sectorsPerCluster;
    uint32_t reservedSectors = hdr->BPB_RsvdSecCnt;
    uint32_t numFATs = hdr->BPB_NumFATs;
    uint32_t FATSize = hdr->BPB_FATSz32;

    // Calculate starting sector of data area (after reserved sectors and FATs)
    uint32_t dataStartSector = reservedSectors + (numFATs * FATSize);
    
    // Calculate total number of clusters
    uint32_t totalSectors = hdr->BPB_TotSec32;
    uint32_t dataSectors = totalSectors - dataStartSector;
    uint32_t totalClusters = dataSectors / sectorsPerCluster;
    
    uint8_t *disk = (uint8_t *)hdr;
    
    // Array to store cluster types
    enum ClusterType *clusterTypes = (enum ClusterType *)calloc(totalClusters + 2, sizeof(enum ClusterType));
    if (!clusterTypes) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
    
    // Step 1: Scan all clusters to categorize them
    for (uint32_t cluster = 2; cluster < totalClusters + 2; cluster++) {
        uint32_t clusterOffset = (dataStartSector * bytesPerSector) + ((cluster - 2) * clusterSize);
        void *clusterData = disk + clusterOffset;
        
        // Check if cluster contains BMP header
        if (is_bmp_header(clusterData)) {
            clusterTypes[cluster] = CLUSTER_BMP_HEADER;
        }
        // Check if cluster contains directory entries
        else if (is_directory_cluster(clusterData, clusterSize)) {
            clusterTypes[cluster] = CLUSTER_DIR_ENTRY;
        }
        // Check if cluster contains BMP data
        else if (is_bmp_header(clusterData, clusterSize)) {
            clusterTypes[cluster] = CLUSTER_BMP_DATA;
        }
        // Default to unused
        else {
            clusterTypes[cluster] = CLUSTER_UNUSED;
        }
    }
    
    // Step 2: Scan directory clusters to extract filenames and recover files
    int recoveredFileCount = 0;
    
    for (uint32_t cluster = 2; cluster < totalClusters + 2; cluster++) {
        if (clusterTypes[cluster] == CLUSTER_DIR_ENTRY) {
            uint32_t clusterOffset = (dataStartSector * bytesPerSector) + ((cluster - 2) * clusterSize);
            struct fat32dent *entries = (struct fat32dent *)(disk + clusterOffset);
            int entriesPerCluster = clusterSize / sizeof(struct fat32dent);
            
            // Find LFN entries and their corresponding 8.3 entries
            for (int i = 0; i < entriesPerCluster; i++) {
                // Skip deleted or empty entries
                if (entries[i].DIR_Name[0] == 0) {
                    continue;
                }
                
                // Check if this is a normal file entry (possibly preceded by LFN entries)
                if (entries[i].DIR_Attr != ATTR_LONG_NAME && 
                    !(entries[i].DIR_Attr & ATTR_DIRECTORY) &&
                    !(entries[i].DIR_Attr & ATTR_VOLUME_ID)) {
                    
                    // Check if filename ends with .BMP (case insensitive)
                    char shortName[13];
                    int j;
                    for (j = 0; j < 8 && entries[i].DIR_Name[j] != ' '; j++) {
                        shortName[j] = entries[i].DIR_Name[j];
                    }
                    shortName[j++] = '.';
                    for (int k = 0; k < 3 && entries[i].DIR_Name[8 + k] != ' '; k++, j++) {
                        shortName[j] = entries[i].DIR_Name[8 + k];
                    }
                    shortName[j] = '\0';
                    
                    // Look for long filename entries that precede this entry
                    int lfnCount = 0;
                    struct fat32_lfn_entry lfnEntries[20]; // Support up to 20 LFN entries
                    
                    int idx = i - 1;
                    while (idx >= 0 && lfnCount < 20 && 
                           entries[idx].DIR_Attr == ATTR_LONG_NAME) {
                        memcpy(&lfnEntries[lfnCount++], &entries[idx], sizeof(struct fat32dent));
                        idx--;
                    }
                    
                    char *filename;
                    if (lfnCount > 0) {
                        filename = extract_long_filename(lfnEntries, lfnCount);
                        if (!filename) {
                            // Fallback to short name if LFN extraction fails
                            filename = strdup(shortName);
                        }
                    } else {
                        filename = strdup(shortName);
                    }
                    
                    if (filename) {
                        // Check if it's a .BMP file (case insensitive)
                        int len = strlen(filename);
                        if (len > 4 && 
                            (strcasecmp(filename + len - 4, ".bmp") == 0)) {
                            
                            // Get start cluster and file size
                            uint32_t startCluster = 
                                ((uint32_t)entries[i].DIR_FstClusHI << 16) | 
                                entries[i].DIR_FstClusLO;
                            uint32_t fileSize = entries[i].DIR_FileSize;
                            
                            // Skip files with invalid clusters or sizes
                            if (startCluster < 2 || startCluster >= totalClusters + 2 || fileSize == 0) {
                                free(filename);
                                continue;
                            }
                            
                            // Check if the start cluster is actually a BMP header
                            if (clusterTypes[startCluster] != CLUSTER_BMP_HEADER) {
                                free(filename);
                                continue;
                            }
                            
                            // Process file immediately, possibly in a child process
                            // Use fork() to parallelize processing for better performance
                            pid_t pid = fork();
                            
                            if (pid == 0) {
                                // Child process - process the file and exit
                                RecoveredFile file;
                                strncpy(file.filename, filename, 255);
                                file.filename[255] = '\0';
                                file.startCluster = startCluster;
                                file.fileSize = fileSize;
                                file.valid = false;
                                file.clusters = NULL;
                                
                                recover_file_data(startCluster, fileSize, disk, clusterSize, 
                                                dataStartSector, bytesPerSector, &file);
                                
                                if (file.valid) {
                                    calculate_sha1(&file, disk, clusterSize, 
                                                dataStartSector, bytesPerSector);
                                }
                                
                                if (file.clusters) {
                                    free(file.clusters);
                                }
                                
                                // Child process exits after processing
                                exit(0);
                            } else if (pid > 0) {
                                // Parent process - keep track of child for cleanup
                                recoveredFileCount++;
                                
                                // Don't let too many child processes run at once
                                // Reap any completed child processes
                                if (recoveredFileCount % 8 == 0) {
                                    while (waitpid(-1, NULL, WNOHANG) > 0);
                                }
                            } else {
                                // Fork failed
                                fprintf(stderr, "Fork failed\n");
                            }
                            free(filename);
                        } else {
                            free(filename);
                        }
                    }
                }
            }
        }
    }
    
    // Wait for all child processes to complete
    while (wait(NULL) > 0);
    
    // Cleanup
    free(clusterTypes);
    
    // file system traversal
    munmap(hdr, hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec);
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
    return hdr;

release:
    if (fd > 0) {
        close(fd);
    }
    exit(1);
}

// Check if a block of data is a BMP file header
int is_bmp_header(const void *data) {
    // Check for BMP signature "BM"
    if (*(uint16_t*)data == BMP_SIGNATURE) {
        // Additional validation can be done by checking other BMP header fields
        const BMPHeader *header = (const BMPHeader*)data;
        const BMPInfoHeader *infoHeader = (const BMPInfoHeader*)(data + 14);
        
        // Reasonable checks for valid BMP files
        if (header->dataOffset >= 54 && 
            infoHeader->headerSize >= 40 && 
            infoHeader->bitsPerPixel == 24 &&  // We know all BMPs are 24-bit
            infoHeader->width > 0 && infoHeader->width < 10000 && 
            infoHeader->height > 0 && infoHeader->height < 10000) {
            return 1;
        }
    }
    return 0;
}

// Check if a given char is valid for a filename (printable ASCII)
bool is_valid_filename_char(unsigned char c) {
    // Printable ASCII chars excluding control chars, spaces, and special chars
    return (c >= 0x21 && c <= 0x7E) || c == 0x20;
}

// Check if a cluster likely contains directory entries
int is_directory_cluster(const void *data, size_t size) {
    const struct fat32dent *entries = (const struct fat32dent *)data;
    int valid_entries = 0;
    int invalid_entries = 0;
    
    // Check multiple directory entries in the cluster
    for (size_t i = 0; i < size / sizeof(struct fat32dent); i++) {
        // Skip deleted entries
        if (entries[i].DIR_Name[0] == DELETED_FLAG || entries[i].DIR_Name[0] == 0) {
            continue;
        }
        
        // Check for long filename attribute or if it's a regular file or directory
        if (entries[i].DIR_Attr == ATTR_LONG_NAME || 
            entries[i].DIR_Attr == ATTR_ARCHIVE || 
            entries[i].DIR_Attr == ATTR_DIRECTORY) {
            
            // For long filename entries, check if it's well-formed
            if (entries[i].DIR_Attr == ATTR_LONG_NAME) {
                const struct fat32_lfn_entry *lfn = (const struct fat32_lfn_entry *)&entries[i];
                
                // LFN has some constraints we can check
                if (lfn->LDIR_Type == 0 && lfn->LDIR_FstClusLO == 0) {
                    // Check if characters in the name seem valid
                    int valid_chars = 0;
                    for (int j = 0; j < 5; j++) {
                        uint16_t c = lfn->LDIR_Name1[j];
                        if (c != 0xFFFF && (c == 0 || is_valid_filename_char(c & 0xFF))) {
                            valid_chars++;
                        }
                    }
                    if (valid_chars > 0) {
                        valid_entries++;
                    } else {
                        invalid_entries++;
                    }
                } else {
                    invalid_entries++;
                }
            } else {
                // Check if the first character of the filename is valid
                if (is_valid_filename_char(entries[i].DIR_Name[0])) {
                    valid_entries++;
                } else {
                    invalid_entries++;
                }
            }
        } else {
            // Unknown attribute
            invalid_entries++;
        }
    }
    
    // If we have more valid than invalid entries, consider it a directory cluster
    return (valid_entries > 0 && valid_entries > invalid_entries) ? 1 : 0;
}

// Helper function to extract long filename from a sequence of LFN entries
char *extract_long_filename(const struct fat32_lfn_entry *lfn_entries, int count) {
    char *filename = (char *)calloc(256, 1); // Max filename length
    if (!filename) return NULL;
    
    int pos = 0;
    
    // Process LFN entries in reverse order (the way they are stored)
    for (int i = count - 1; i >= 0; i--) {
        const struct fat32_lfn_entry *lfn = &lfn_entries[i];
        
        // Extract characters from Name1
        for (int j = 0; j < 5; j++) {
            uint16_t c = lfn->LDIR_Name1[j];
            if (c == 0 || c == 0xFFFF) break;
            filename[pos++] = c & 0xFF; // Just take the lower byte (ASCII)
        }
        
        // Extract characters from Name2
        for (int j = 0; j < 6; j++) {
            uint16_t c = lfn->LDIR_Name2[j];
            if (c == 0 || c == 0xFFFF) break;
            filename[pos++] = c & 0xFF;
        }
        
        // Extract characters from Name3
        for (int j = 0; j < 2; j++) {
            uint16_t c = lfn->LDIR_Name3[j];
            if (c == 0 || c == 0xFFFF) break;
            filename[pos++] = c & 0xFF;
        }
    }
    
    filename[pos] = '\0';
    return filename;
}

// Check if a cluster contains BMP image data (optimized for 24-bit BMPs)
int is_bmp_data(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    
    // Look for patterns common in 24-bit BMP data
    // In 24-bit BMP files, each pixel takes 3 bytes (BGR order)
    // Check for reasonable color values and patterns
    
    // Count smooth color transitions (adjacent pixels with similar colors)
    int smooth_transitions = 0;
    for (size_t i = 0; i < size - 6; i += 3) {
        int r1 = bytes[i + 2];
        int g1 = bytes[i + 1];
        int b1 = bytes[i];
        
        int r2 = bytes[i + 5];
        int g2 = bytes[i + 4];
        int b2 = bytes[i + 3];
        
        // Calculate color difference between adjacent pixels
        int diff = abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2);
        
        // If the difference is within a threshold, consider it a smooth transition
        if (diff < 100) {
            smooth_transitions++;
        }
    }
    
    // If more than 40% of transitions are smooth, it's likely image data
    return (smooth_transitions > (size / 15)); 
}

// Recover file data starting from a given cluster
void recover_file_data(uint32_t startCluster, uint32_t fileSize, uint8_t *disk, 
                      uint32_t clusterSize, uint32_t dataStartSector, uint32_t bytesPerSector,
                      RecoveredFile *file) {
    // Calculate how many clusters are needed for this file
    uint32_t clustersNeeded = (fileSize + clusterSize - 1) / clusterSize;
    
    // Allocate memory for cluster list
    file->clusters = (uint32_t *)malloc(clustersNeeded * sizeof(uint32_t));
    if (!file->clusters) {
        file->valid = false;
        return;
    }
    
    file->clusterCount = clustersNeeded;
    file->clusters[0] = startCluster;
    
    // First try contiguous clusters (most common case)
    bool contiguous = true;
    for (uint32_t i = 1; i < clustersNeeded; i++) {
        uint32_t nextCluster = startCluster + i;
        uint32_t clusterOffset = (dataStartSector * bytesPerSector) + ((nextCluster - 2) * clusterSize);
        
        // Verify this is either a BMP data cluster or the right type of cluster for this position
        if (i == 0) {
            // First cluster should be a BMP header
            if (!is_bmp_header(disk + clusterOffset)) {
                contiguous = false;
                break;
            }
        } else {
            // Subsequent clusters should be BMP data
            if (!is_bmp_data(disk + clusterOffset, clusterSize)) {
                contiguous = false;
                break;
            }
        }
        
        file->clusters[i] = nextCluster;
    }
    
    // If clusters aren't contiguous, try a more sophisticated approach
    if (!contiguous && clustersNeeded > 1) {
        // Reset and try a search based on BMP data signature
        file->clusters[0] = startCluster;
        uint32_t currentCluster = startCluster;
        uint32_t remainingClusters = clustersNeeded - 1;
        uint32_t currentIndex = 0;
        
        // Scan nearby clusters for BMP data that might belong to this file
        const int SEARCH_RANGE = 100; // Number of clusters to search ahead
        while (remainingClusters > 0 && currentIndex < clustersNeeded) {
            bool found = false;
            
            // Try to find the next cluster
            for (uint32_t offset = 1; offset <= SEARCH_RANGE && !found; offset++) {
                uint32_t candidateCluster = currentCluster + offset;
                uint32_t clusterOffset = (dataStartSector * bytesPerSector) + 
                                      ((candidateCluster - 2) * clusterSize);
                
                // Check if this cluster contains BMP data
                if (is_bmp_data(disk + clusterOffset, clusterSize)) {
                    currentIndex++;
                    file->clusters[currentIndex] = candidateCluster;
                    currentCluster = candidateCluster;
                    remainingClusters--;
                    found = true;
                }
            }
            
            // If we couldn't find a next cluster, give up
            if (!found) {
                // Fall back to contiguous allocation for remaining clusters
                for (uint32_t i = currentIndex + 1; i < clustersNeeded; i++) {
                    file->clusters[i] = file->clusters[currentIndex] + (i - currentIndex);
                }
                break;
            }
        }
    }
    
    file->valid = true;
}

// Write recovered file to temp file and calculate its SHA1
void calculate_sha1(RecoveredFile *file, uint8_t *disk, uint32_t clusterSize,
                   uint32_t dataStartSector, uint32_t bytesPerSector) {
    if (!file->valid || !file->clusters) {
        return;
    }
    
    char tempFileName[64];
    snprintf(tempFileName, sizeof(tempFileName), "/tmp/fsrecov_temp_%u", file->startCluster);
    
    FILE *fp = fopen(tempFileName, "wb");
    if (!fp) {
        perror("Failed to create temp file");
        return;
    }
    
    // Write all clusters to the temp file
    uint32_t remainingSize = file->fileSize;
    
    for (uint32_t i = 0; i < file->clusterCount && remainingSize > 0; i++) {
        uint32_t cluster = file->clusters[i];
        uint32_t clusterOffset = (dataStartSector * bytesPerSector) + 
                               ((cluster - 2) * clusterSize); // Clusters start at 2
        
        uint32_t bytesToWrite = (remainingSize > clusterSize) ? clusterSize : remainingSize;
        fwrite(disk + clusterOffset, 1, bytesToWrite, fp);
        remainingSize -= bytesToWrite;
    }
    
    fclose(fp);
    
    // Call sha1sum to calculate the checksum
    char command[512];
    snprintf(command, sizeof(command), "sha1sum %s", tempFileName);
    
    FILE *sha1Fp = popen(command, "r");
    if (!sha1Fp) {
        perror("Failed to run sha1sum");
        unlink(tempFileName);
        return;
    }
    
    char sha1sum[41];
    if (fscanf(sha1Fp, "%40s", sha1sum) == 1) {
        printf("%s  %s\n", sha1sum, file->filename);
    }
    
    pclose(sha1Fp);
    unlink(tempFileName); // Delete temp file
}
