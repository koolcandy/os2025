#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h> // For isprint

// Define FAT32 directory entry constants
#define FAT32_DIR_ENTRY_SIZE 32
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID) // 0x0F

// Define entry types for parsing result
typedef enum {
    ENTRY_TYPE_FREE,
    ENTRY_TYPE_DELETED,
    ENTRY_TYPE_LFN,
    ENTRY_TYPE_SHORT,
    ENTRY_TYPE_VOLUME_ID,
    ENTRY_TYPE_DIRECTORY,
    ENTRY_TYPE_UNKNOWN
} EntryType;

// Struct to hold parsed information from a single entry
typedef struct {
    EntryType type;
    uint8_t attributes;
    // For LFN entries:
    uint8_t lfn_sequence; // Sequence number (1-based), high bit 0x40 if last entry
    uint8_t lfn_checksum;
    uint16_t lfn_name1[5]; // 5 UTF-16 characters
    uint16_t lfn_name2[6]; // 6 UTF-16 characters
    uint16_t lfn_name3[2]; // 2 UTF-16 characters
    // For Short entries:
    char short_name[12]; // 8.3 format, null-terminated
    uint32_t first_cluster_low; // Lower 16 bits
    uint32_t first_cluster_high; // Upper 16 bits
    uint32_t file_size;
} ParsedEntryInfo;

// Helper function to calculate the LFN checksum from an 11-byte short filename
uint8_t calculate_lfn_checksum(const unsigned char *short_name_bytes) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) << 7) + (sum >> 1) + short_name_bytes[i];
    }
    return sum;
}

// Function to parse a single 32-byte directory entry
void parse_single_entry(const unsigned char *entry_data, ParsedEntryInfo *info) {
    // Initialize info struct
    memset(info, 0, sizeof(ParsedEntryInfo));

    // Check first byte
    if (entry_data[0] == 0xE5) {
        info->type = ENTRY_TYPE_DELETED;
        return;
    }
    if (entry_data[0] == 0x00) {
        info->type = ENTRY_TYPE_FREE;
        return;
    }

    info->attributes = entry_data[0x0B];

    // Check attribute for LFN
    if (info->attributes == FAT32_ATTR_LFN) {
        info->type = ENTRY_TYPE_LFN;
        info->lfn_sequence = entry_data[0];
        info->lfn_checksum = entry_data[0x0D];
        // Copy LFN name parts (UTF-16LE)
        memcpy(info->lfn_name1, entry_data + 1, 10); // 5 chars * 2 bytes
        memcpy(info->lfn_name2, entry_data + 14, 12); // 6 chars * 2 bytes
        memcpy(info->lfn_name3, entry_data + 28, 4); // 2 chars * 2 bytes
    }
    // Check attribute for Short Entry (not LFN, not Volume ID, not Directory)
    // A file entry typically has ATTR_ARCHIVE (0x20) set.
    // A directory entry has ATTR_DIRECTORY (0x10) set.
    else if (!(info->attributes & FAT32_ATTR_VOLUME_ID)) { // Not a volume ID entry
         if (info->attributes & FAT32_ATTR_DIRECTORY) {
            info->type = ENTRY_TYPE_DIRECTORY;
         } else {
            info->type = ENTRY_TYPE_SHORT; // Assume it's a file or other short entry type
         }

        // Copy short name (first 11 bytes)
        memcpy(info->short_name, entry_data, 11);
        info->short_name[11] = '\0'; // Null terminate

        // Parse cluster and size (Little Endian)
        // First Cluster High 16 bits at offset 0x14
        info->first_cluster_high = (uint32_t)entry_data[0x15] << 8 | entry_data[0x14];
        // First Cluster Low 16 bits at offset 0x1A
        info->first_cluster_low = (uint32_t)entry_data[0x1B] << 8 | entry_data[0x1A];
        // File Size at offset 0x1C (4 bytes)
        info->file_size = (uint32_t)entry_data[0x1F] << 24 |
                          (uint32_t)entry_data[0x1E] << 16 |
                          (uint32_t)entry_data[0x1D] << 8 |
                          entry_data[0x1C];
    } else {
        info->type = ENTRY_TYPE_UNKNOWN; // Could be Volume ID, etc.
    }
}

// Helper function to convert a UTF-16LE character to a printable ASCII char (or '?')
char utf16le_to_printable_ascii(uint16_t utf16_char) {
    // Allow a wider range of printable ASCII, including underscores and hyphens, as mentioned in problem
    if ((utf16_char >= 0x20 && utf16_char <= 0x7E)
        || utf16_char == '_' || utf16_char == '-') {
        return (char)utf16_char;
    }
    return '?'; // Replace non-printable/non-ASCII with '?'
}

// Helper function to reconstruct long filename from LFN entries (simplified for printable ASCII)
// This function assumes LFN entries are processed in reverse order (last part first)
// and expects a short entry to finalize the sequence.
// In a real scenario, you'd collect all LFN parts and then process them.
// For this example, we'll just demonstrate parsing the known sequence.
void reconstruct_lfn_from_sequence(const ParsedEntryInfo lfn_entries[], int num_lfn_entries, const ParsedEntryInfo *short_entry, char *long_filename_buffer, size_t buffer_size) {
    if (num_lfn_entries == 0 || short_entry == NULL || short_entry->type != ENTRY_TYPE_SHORT) {
        // Fallback to short name if LFN reconstruction is not possible
        strncpy(long_filename_buffer, short_entry ? short_entry->short_name : "InvalidEntry", buffer_size - 1);
        // Replace spaces in short name with underscores for better filename compatibility if needed
        for (int i = 0; i < buffer_size - 1 && long_filename_buffer[i] != '\0'; ++i) {
            if (long_filename_buffer[i] == ' ') long_filename_buffer[i] = '_';
        }
        // Trim trailing underscores/spaces/dots from short name? (FAT short name rules)
        // For simplicity, just null terminate after the 11 bytes or the first space.
        for (int i = 0; i < 11 && i < buffer_size - 1; ++i) {
             if (long_filename_buffer[i] == ' ') {
                 long_filename_buffer[i] = '\0';
                 break;
             }
        }
         long_filename_buffer[buffer_size - 1] = '\0'; // Ensure null termination
        return;
    }
    // Validate checksum (check against the checksum in the first LFN entry, seq 1)
    uint8_t calculated_checksum = calculate_lfn_checksum((const unsigned char *)short_entry->short_name);
    // Find the LFN entry with sequence number 1 to get the expected checksum
    uint8_t expected_checksum = 0;
    bool found_seq1 = false;
    for(int i = 0; i < num_lfn_entries; ++i) {
        if ((lfn_entries[i].lfn_sequence & 0xBF) == 1) {
            expected_checksum = lfn_entries[i].lfn_checksum;
            found_seq1 = true;
            break;
        }
    }
    if (found_seq1 && calculated_checksum != expected_checksum) {
        fprintf(stderr, "Checksum mismatch! Expected %02x, calculated %02x\n", expected_checksum, calculated_checksum);
        // In a real recovery, this sequence might be invalid.
        // You might choose to *not* reconstruct the LFN if checksum fails,
        // and instead fallback to the short name.
        // For this example, we proceed with LFN reconstruction despite mismatch.
    } else if (!found_seq1) {
         fprintf(stderr, "Warning: Could not find LFN entry with sequence 1 to verify checksum.\n");
    }
    // Allocate space for full UTF-16LE name (max 13 chars per LFN entry * num_lfn_entries)
    // The max LFN is 255 chars, which needs 20 LFN entries (255 / 13 = 19.6 -> 20 entries).
    // So max num_lfn_entries is around 20. Max UTF-16 chars = 20 * 13 = 260.
    uint16_t full_utf16_name[260 + 1]; // Max LFN chars + null terminator
    int utf16_len = 0;
    // Process LFN entries in correct sequence order (from 1 up to num_lfn_entries)
    // Assuming the input array lfn_entries is already sorted by sequence number.
    // My main function uses ordered_lfn, which is sorted by seq.
    for (int i = 0; i < num_lfn_entries; ++i) { // Iterate forwards (correct sequence order)
        const ParsedEntryInfo *lfn = &lfn_entries[i];
        // Check sequence number validity (optional but good practice)
        int current_seq = lfn->lfn_sequence & 0xBF;
        if (current_seq != (i + 1)) {
             fprintf(stderr, "Warning: LFN entry at index %d has sequence %d, expected %d.\n", i, current_seq, i + 1);
             // Decide how to handle out-of-order or missing sequences.
             // For now, just warn and process the data as is.
        }
        // Append name parts, stopping at null terminator (0x0000) or 0xFFFF padding
        for (int j = 0; j < 5; ++j) {
            if (lfn->lfn_name1[j] == 0x0000 || lfn->lfn_name1[j] == 0xFFFF) goto end_lfn_copy_this_entry;
            full_utf16_name[utf16_len++] = lfn->lfn_name1[j];
        }
        for (int j = 0; j < 6; ++j) {
             if (lfn->lfn_name2[j] == 0x0000 || lfn->lfn_name2[j] == 0xFFFF) goto end_lfn_copy_this_entry;
            full_utf16_name[utf16_len++] = lfn->lfn_name2[j];
        }
        for (int j = 0; j < 2; ++j) {
             if (lfn->lfn_name3[j] == 0x0000 || lfn->lfn_name3[j] == 0xFFFF) goto end_lfn_copy_this_entry;
            full_utf16_name[utf16_len++] = lfn->lfn_name3[j];
        }
        end_lfn_copy_this_entry:; // Label to jump to after processing parts of one LFN entry
    }
    full_utf16_name[utf16_len] = 0x0000; // Ensure null termination of UTF-16 string
    // Convert UTF-16LE to printable ASCII
    int buffer_pos = 0;
    for (int i = 0; i < utf16_len && buffer_pos < buffer_size - 1; ++i) {
        long_filename_buffer[buffer_pos++] = utf16le_to_printable_ascii(full_utf16_name[i]);
    }
    long_filename_buffer[buffer_pos] = '\0';
}


// Example usage with the provided dump data
int main() {
    // The provided hex dump data representing 3 directory entries (3 * 32 bytes)
    const unsigned char directory_dump[] = {
        // Entry 1 (at 00025ae0) - LFN sequence 2 (last)
        0x42, 0x50, 0x00, 0x43, 0x00, 0x70, 0x00, 0x2e, 0x00, 0x62, 0x00, 0x0f, 0x89, 0x6d, 0x00, 0x70,
        0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        // Entry 2 (at 00025b00) - LFN sequence 1 (first)
        0x01, 0x30, 0x00, 0x4d, 0x00, 0x31, 0x00, 0x35, 0x00, 0x43, 0x00, 0x0f, 0x89, 0x77, 0x00, 0x47,
        0x00, 0x31, 0x00, 0x79, 0x00, 0x50, 0x00, 0x33, 0x00, 0x00, 0x00, 0x32, 0x00, 0x55, 0x00, 0x00,
        // Entry 3 (at 00025b20) - Short entry
        0x30, 0x4d, 0x31, 0x35, 0x43, 0x57, 0x7e, 0x31, 0x42, 0x4d, 0x50, 0x20, 0x00, 0x64, 0x2b, 0x5a,
        0xac, 0x50, 0xac, 0x50, 0x00, 0x00, 0x2b, 0x5a, 0xac, 0x50, 0x69, 0x15, 0x36, 0x77, 0x07, 0x00
    };

    ParsedEntryInfo entry_info[3];
    ParsedEntryInfo lfn_parts[2]; // To store LFN entries in sequence order (1, 2)
    int lfn_count = 0;
    const ParsedEntryInfo *short_entry = NULL;

    // Process each 32-byte entry in the dump
    for (int i = 0; i < sizeof(directory_dump) / FAT32_DIR_ENTRY_SIZE; ++i) {
        const unsigned char *current_entry_data = directory_dump + i * FAT32_DIR_ENTRY_SIZE;
        parse_single_entry(current_entry_data, &entry_info[i]);

        printf("--- Entry %d ---\n", i + 1);
        switch (entry_info[i].type) {
            case ENTRY_TYPE_LFN:
                printf("Type: LFN\n");
                printf("Sequence: %02x (Is last: %s)\n", entry_info[i].lfn_sequence, (entry_info[i].lfn_sequence & 0x40) ? "Yes" : "No");
                printf("Checksum: %02x\n", entry_info[i].lfn_checksum);
                // Store LFN parts based on sequence number
                if (!(entry_info[i].lfn_sequence & 0x40)) { // Not the last part
                    int seq = entry_info[i].lfn_sequence;
                     if (seq >= 1 && seq <= 2 && lfn_count < 2) { // Basic bounds check
                        lfn_parts[seq - 1] = entry_info[i]; // Store at index seq-1
                        lfn_count++;
                     } else {
                         fprintf(stderr, "Unexpected LFN sequence number or count: %d\n", seq);
                     }
                } else { // Is the last part
                    int seq = entry_info[i].lfn_sequence & 0xBF; // Mask out last bit
                    if (seq >= 1 && seq <= 2 && lfn_count < 2) { // Basic bounds check
                         lfn_parts[seq - 1] = entry_info[i]; // Store at index seq-1
                         lfn_count++;
                    } else {
                        fprintf(stderr, "Unexpected LFN last sequence number or count: %d\n", seq);
                    }
                }

                break;
            case ENTRY_TYPE_SHORT:
                printf("Type: Short\n");
                printf("Short Name: %.11s\n", entry_info[i].short_name);
                printf("Attributes: %02x\n", entry_info[i].attributes);
                printf("First Cluster Low: %04x\n", entry_info[i].first_cluster_low);
                printf("First Cluster High: %04x\n", entry_info[i].first_cluster_high);
                printf("File Size: %u\n", entry_info[i].file_size);
                short_entry = &entry_info[i]; // Found the short entry
                break;
            case ENTRY_TYPE_FREE:
                printf("Type: Free\n");
                break;
            case ENTRY_TYPE_DELETED:
                printf("Type: Deleted\n");
                break;
            case ENTRY_TYPE_DIRECTORY:
                 printf("Type: Directory (Short Entry)\n");
                 printf("Short Name: %.11s\n", entry_info[i].short_name);
                 printf("Attributes: %02x\n", entry_info[i].attributes);
                 printf("First Cluster Low: %04x\n", entry_info[i].first_cluster_low);
                 printf("First Cluster High: %04x\n", entry_info[i].first_cluster_high);
                 printf("File Size: %u (Directories always have size 0)\n", entry_info[i].file_size);
                 break;
            default:
                printf("Type: Unknown (%02x)\n", current_entry_data[0x0B]);
                break;
        }
    }

    printf("\n--- Reconstructed File Info ---\n");
    if (short_entry && lfn_count > 0) {
        char long_filename[260]; // Max path length
        // Note: lfn_parts array needs to be ordered correctly for reconstruction.
        // In the dump, entry 1 (index 0) is seq 2, entry 2 (index 1) is seq 1.
        // So, lfn_parts[0] should be entry 2 info, lfn_parts[1] should be entry 1 info.
        ParsedEntryInfo ordered_lfn[2];
        // Find LFN seq 1 and seq 2 from entry_info array
        int ordered_count = 0;
        for(int i=0; i<3; ++i) {
            if(entry_info[i].type == ENTRY_TYPE_LFN) {
                int seq = entry_info[i].lfn_sequence & 0xBF;
                if (seq >= 1 && seq <= 2) {
                    ordered_lfn[seq - 1] = entry_info[i];
                    ordered_count++;
                }
            }
        }

        if (ordered_count == lfn_count) { // Ensure we found all expected LFN parts
             reconstruct_lfn_from_sequence(ordered_lfn, lfn_count, short_entry, long_filename, sizeof(long_filename));
             printf("Long Filename: %s\n", long_filename);
        } else {
             fprintf(stderr, "Could not collect all expected LFN parts (%d/%d)\n", ordered_count, lfn_count);
             // Fallback to short name if LFN reconstruction failed
             strncpy(long_filename, short_entry->short_name, sizeof(long_filename) - 1);
             long_filename[sizeof(long_filename)-1] = '\0';
             printf("Using Short Filename: %s\n", long_filename);
        }


        uint32_t full_cluster_id = (short_entry->first_cluster_high << 16) | short_entry->first_cluster_low;
        printf("First Cluster ID: %u (0x%x)\n", full_cluster_id, full_cluster_id);
        printf("File Size: %u bytes\n", short_entry->file_size);

        // Note: The file size (7 bytes) derived from this specific short entry dump (00025b20)
        // is likely for a *different* tiny file that happens to be next to the LFNs of the BMP.
        // The actual BMP file size (1035054 bytes) would come from its *correct* short directory entry.
        // You would need to find the *correct* short entry for the BMP you are trying to recover.
        // The task is to find *any* valid directory entries and *any* valid BMP headers,
        // and then try to match them up.
        // The example dump just shows what a directory entry block *looks like*, not necessarily
        // the exact entry for the BMP header shown elsewhere in the problem description.

    } else if (short_entry) {
         printf("Found Short Entry but no associated LFNs.\n");
         printf("Short Filename: %s\n", short_entry->short_name);
         uint32_t full_cluster_id = (short_entry->first_cluster_high << 16) | short_entry->first_cluster_low;
         printf("First Cluster ID: %u (0x%x)\n", full_cluster_id, full_cluster_id);
         printf("File Size: %u bytes\n", short_entry->file_size);
    }
     else {
        printf("No complete file entry sequence found in the dump.\n");
    }


    return 0;
}
