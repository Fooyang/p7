#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>


char *disk_path;
char *mount_point;
int file_size;
int fd;
char *mem;
struct wfs_sb *sb;
struct wfs_inode *root_inode;

static int read_inode(int inode_index, struct wfs_inode *inode) {
    // Open the disk image in read mode
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Read the superblock from the disk file
    struct wfs_sb superblock;
    if (pread(fd, &superblock, sizeof(struct wfs_sb), 0) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Calculate the offset of the inode bitmap
    off_t inode_bitmap_offset = superblock.i_bitmap_ptr + ((inode_index / 8) * sizeof(char));

    // Read the inode bitmap
    char inode_bitmap;
    if (pread(fd, &inode_bitmap, sizeof(char), inode_bitmap_offset) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Check if the inode index is valid
    if (inode_index >= superblock.num_inodes) {
        fprintf(stderr, "Invalid inode index\n");
        close(fd);
        return -1;
    }

    // Check if the inode is allocated in the bitmap
    if (!(inode_bitmap & (1 << (inode_index % 8)))) {
        // Inode not allocated
        close(fd);
        return -1;
    }

    // Calculate the offset of the inode on disk
    off_t inode_offset = superblock.i_blocks_ptr + (inode_index * sizeof(struct wfs_inode));

    // Read the inode from the disk image
    if (pread(fd, inode, sizeof(struct wfs_inode), inode_offset) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int get_inode_index(const char *path) {
    struct wfs_sb superblock;
    char *mutable_path = strdup(path); // Make a mutable copy of the path
    if (mutable_path == NULL) {
        perror("strdup");
        return -1;
    }

    // Open the disk image in read mode
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        free(mutable_path); // Free the allocated memory
        return -1;
    }

    // Read the superblock to get the root inode index
    // if (pread(fd, &superblock, sizeof(struct wfs_sb), 0) != sizeof(struct wfs_sb)) {
    //     perror("pread");
    //     close(fd);
    //     free(mutable_path); // Free the allocated memory
    //     return -1;
    // }

    // Initialize inode index with the root inode index
    int inode_index = 0;

    // Parse the path and traverse through the directories
    char *token = strtok(mutable_path, "/");
    while (token != NULL) {
        // Read the inode corresponding to the current index
        struct wfs_inode inode;
        if (read_inode(inode_index, &inode) == -1) {
            close(fd);
            free(mutable_path); // Free the allocated memory
            return -1;
        }

        // Search for the token in the directory entries
        int found = 0;
        for (int i = 0; i < D_BLOCK; i++) {
            struct wfs_dentry directory_entry;
            if (pread(fd, &directory_entry, sizeof(struct wfs_dentry), inode.blocks[i]) != sizeof(struct wfs_dentry)) {
                perror("pread");
                close(fd);
                free(mutable_path); // Free the allocated memory
                return -1;
            }

            // Compare the directory entry name with the token
            if (strcmp(directory_entry.name, token) == 0) {
                inode_index = directory_entry.num; // Update inode index
                found = 1;
                break;
            }
        }

        // If the directory entry was not found
        if (!found) {
            fprintf(stderr, "Directory not found: %s\n", token);
            close(fd);
            free(mutable_path); // Free the allocated memory
            return -1;
        }

        // Get the next token
        token = strtok(NULL, "/");
    }

    close(fd);
    free(mutable_path); // Free the allocated memory
    return inode_index;
}

// Function to get attributes of a file or directory
static int wfs_getattr(const char *path, struct stat *stbuf) {
    // Initialize the struct stat with 0s
    memset(stbuf, 0, sizeof(struct stat));

    // Get the inode index corresponding to the path
    int inode_index = get_inode_index(path);
    if (inode_index == -1) {
        // Path doesn't exist
        return -ENOENT;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1) {
        return -EIO;
    }

    // Set common attributes for both files and directories
    stbuf->st_uid = inode.uid; // Owner user ID
    stbuf->st_gid = inode.gid; // Owner group ID
    stbuf->st_atime = inode.atim; // Last access time
    stbuf->st_mtime = inode.mtim; // Last modification time

    // Check if the inode represents a directory
    if (S_ISDIR(inode.mode)) {
        // Set attributes for the directory
        stbuf->st_mode = S_IFDIR | inode.mode; // Directory with permissions
        stbuf->st_nlink = 2; // Number of hard links (for simplicity, we assume it's always 2)
        return 0;
    }

    // Set attributes for a regular file
    stbuf->st_mode = S_IFREG | inode.mode; // Regular file with permissions
    stbuf->st_nlink = 1; // Number of hard links (for simplicity, we assume it's always 1)
    stbuf->st_size = inode.size; // File size in bytes
    return 0;
}


static int make(const char* path, mode_t mode) {
    // make a new inode of the mode specified 
    

}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{

    return 0; // Success
}

static int wfs_mkdir(const char* path, mode_t mode)
{

    return 0; // Return 0 on success
}

static int wfs_unlink(const char* path)
{
    return 0; // Return 0 on success
}

static int wfs_rmdir(const char* path)
{
    return 0; // Return 0 on success
}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    // Get the inode index for the given path
    int inode_index = get_inode_index(path);
    if (inode_index == -1) {
        // Path doesn't exist
        return -ENOENT;
    }

    // Open the disk image in read mode
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -EIO;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1) {
        close(fd); // Close the file descriptor before returning
        return -EIO;
    }

    // Check if the inode represents a directory
    if (S_ISDIR(inode.mode)) {
        close(fd); // Close the file descriptor before returning
        return -EISDIR; // Not a regular file
    }

    // Read file contents
    ssize_t bytes_read = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (inode.blocks[i] == 0) {
            break; // No more blocks
        }
        // Read block contents
        ssize_t block_size = (offset < inode.size) ? ((inode.size - offset < BLOCK_SIZE) ? inode.size - offset : BLOCK_SIZE) : 0;
        ssize_t bytes = pread(fd, buf + bytes_read, block_size, inode.blocks[i] * BLOCK_SIZE + offset);
        if (bytes == -1) {
            perror("pread");
            close(fd); // Close the file descriptor before returning
            return -EIO;
        }
        bytes_read += bytes;
        if (bytes < BLOCK_SIZE) {
            break; // Reached end of file
        }
    }

    close(fd); // Close the file descriptor before returning
    return bytes_read;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    // // Get the inode index for the given file path
    // int inode_index = get_inode_index(path);
    // if (inode_index == -1) {
    //     return -ENOENT; // File not found
    // }

    // // Open the disk image in read mode
    // int fd = open(disk_path, O_RDONLY);
    // if (fd == -1) {
    //     perror("open");
    //     return -EIO;
    // }

    // // Read the inode information
    // struct wfs_inode inode;
    // if (read_inode(inode_index, &inode) == -1) {
    //     return -EIO; // I/O error
    // }

    // // Check if the inode represents a regular file
    // if (!(inode.mode & S_IFREG)) {
    //     return -EISDIR; // Not a regular file
    // }

    // // Determine the block index and offset within the file
    // off_t block_index = offset / BLOCK_SIZE;
    // off_t block_offset = offset % BLOCK_SIZE;

    // // Iterate over the blocks to write the data
    // size_t bytes_written = 0;
    // while (bytes_written < size) {
    //     // Calculate the block size to write
    //     size_t remaining_size = size - bytes_written;
    //     size_t block_size = remaining_size < BLOCK_SIZE - block_offset ? remaining_size : BLOCK_SIZE - block_offset;

    //     // Allocate a new block if needed
    //     if (inode.blocks[block_index] == 0) {
    //         // Find a free block
    //         off_t free_block_index = find_free_block();
    //         if (free_block_index == -1) {
    //             return -ENOSPC; // No space left on device
    //         }
    //         // Mark the block as used in the data bitmap
    //         if (mark_block_used(free_block_index) == -1) {
    //             return -EIO; // I/O error
    //         }
    //         // Update the inode with the new block
    //         inode.blocks[block_index] = free_block_index;
    //     }

    //     // Write data to the block
    //     off_t block_offset_in_disk = inode.blocks[block_index] * BLOCK_SIZE + block_offset;
    //     ssize_t bytes_written_to_block = pwrite(fd, buf + bytes_written, block_size, block_offset_in_disk);
    //     if (bytes_written_to_block == -1) {
    //         perror("pwrite");
    //         return -EIO; // I/O error
    //     }

    //     // Update the number of bytes written
    //     bytes_written += bytes_written_to_block;

    //     // Move to the next block
    //     block_index++;
    //     block_offset = 0; // Reset the block offset for subsequent blocks
    // }

    // // Update the inode size if necessary
    // if (offset + size > inode.size) {
    //     inode.size = offset + size;
    // }

    // // Write the updated inode to disk
    // if (write_inode(inode_index, &inode) == -1) {
    //     return -EIO; // I/O error
    // }

    // return size; // Return the number of bytes written on success
    return 1;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
{
    // Get the inode index for the given directory path
    int inode_index = get_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT; // Directory not found
    }

    // Read the inode information
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1) {
        return -EIO; // I/O error
    }

    // Check if the inode represents a directory
    if (!(inode.mode & S_IFDIR)) {
        return -ENOTDIR; // Not a directory
    }

    // Open the directory corresponding to the inode
    DIR* dir = opendir(path);
    if (dir == NULL) {
        perror("opendir");
        exit(EXIT_FAILURE);
    }

    // Read directory entries and fill the buffer
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Add the directory entry to the buffer
        if (filler(buf, entry->d_name, NULL, 0) != 0) {
            return -ENOMEM; // Buffer full
        }
    }

    // Close the directory
    closedir(dir);

    return 0; // Success
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char *argv[])
{
    // Check for correct number of arguments
    if (argc < 4)
    {
        fprintf(stderr, "Usage: %s disk_path [FUSE options] mount_point\n", argv[0]);
        return EXIT_FAILURE;
    }

    disk_path = argv[1];          // Get the disk path from command-line arguments
    mount_point = argv[argc - 1]; // Get the mountpoint from command-line arguments
    char *new_args[argc-1];
    int new_count = 0;
    for (int i = 0; i < argc; i++)
    {
        if (i == 1) {
            continue;
        }
        new_args[new_count] = argv[i];
        new_count++;
    }
    new_args[new_count] = NULL;

    // mmap
    fd = open(disk_path, O_RDWR);
    file_size = lseek(fd, 0, SEEK_END);
    mem = mmap(0, file_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    sb = mem;
    root_inode = (struct wfs_inode *)((char *)mem + sizeof(struct wfs_sb));

    //     // Start the FUSE event loop with the provided callback functions
    int ret = fuse_main(argc-1, new_args, &ops, NULL);

    return ret;
}