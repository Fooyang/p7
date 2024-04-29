#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

char *disk_path;
char *mount_point;

static int read_inode(int inode_index, struct wfs_inode *inode) {
    struct wfs_sb superblock;

    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    // Read the superblock from the disk file
    if (pread(fd, &superblock, sizeof(struct wfs_sb), 0) == -1) {
        perror("pread");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Calculate the offset of the inode bitmap
    off_t inode_bitmap_offset = superblock.i_bitmap_ptr + ((inode_index / 8) * sizeof(char));

    // Read the inode bitmap
    char inode_bitmap;
    if (pread(fd, &inode_bitmap, sizeof(char), inode_bitmap_offset) == -1) {
        perror("pread");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Check if the inode index is valid
    if (inode_index >= superblock.num_inodes) {
        fprintf(stderr, "Invalid inode index\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Check if the inode is allocated in the bitmap
    if (!(inode_bitmap & (1 << (inode_index % 8)))) {
        // Inode not allocated
        close(fd);
        return -1;
    }

    // Calculate the offset of the inode on disk
    off_t superblock_size = sizeof(struct wfs_sb);
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

static int write_inode(int inode_index, struct wfs_inode *inode) {
    int fd = open(disk_path, O_RDWR);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Read the superblock from the disk image
    struct wfs_sb superblock;
    if (pread(fd, &superblock, sizeof(struct wfs_sb), 0) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Calculate the offset of the inode on disk
    off_t superblock_size = sizeof(struct wfs_sb);
    off_t inode_offset = superblock.i_blocks_ptr + (inode_index * sizeof(struct wfs_inode));

    // Move the file cursor to the inode offset
    if (lseek(fd, inode_offset, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        return -1;
    }

    // Write the inode to disk
    ssize_t bytes_written = write(fd, inode, sizeof(struct wfs_inode));
    if (bytes_written != sizeof(struct wfs_inode)) {
        perror("write");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int get_inode_index(const char *path) {
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    struct wfs_sb superblock;
    if (pread(fd, &superblock, sizeof(struct wfs_sb), 0) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Read the inode bitmap
    char inode_bitmap[superblock.num_inodes / 8]; // 1 bit per inode
    if (pread(fd, &inode_bitmap, sizeof(inode_bitmap), superblock.i_bitmap_ptr * BLOCK_SIZE) == -1) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Traverse the inode blocks
    for (off_t i = 0; i < superblock.num_inodes; i++) {
        // Check if the inode is allocated
        if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) {
            continue; // Inode not allocated, skip
        }

        // Read the inode
        struct wfs_inode inode;
        off_t inode_offset = superblock.i_blocks_ptr * BLOCK_SIZE + i * sizeof(struct wfs_inode);
        if (pread(fd, &inode, sizeof(struct wfs_inode), inode_offset) == -1) {
            perror("pread");
            close(fd);
            return -1;
        }

        // Check if the inode is a directory
        if ((inode.mode & S_IFMT) != S_IFDIR) {
            continue; // Not a directory, skip
        }

        // Traverse the directory entries
        for (int j = 0; j < D_BLOCK; j++) {
            struct wfs_dentry entry;
            off_t entry_offset = inode.blocks[j] * BLOCK_SIZE + sizeof(struct wfs_dentry);
            if (pread(fd, &entry, sizeof(struct wfs_dentry), entry_offset) == -1) {
                perror("pread");
                close(fd);
                return -1;
            }

            // Check if the entry matches the path
            if (strcmp(entry.name, path + 1) == 0) { // Skip the leading '/'
                close(fd);
                return entry.num; // Return the inode number
            }
        }
    }

    // Path not found
    close(fd);
    return -1;
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
    return 0; // Return 0 on success
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    return 0; // Return 0 on success
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
{
    return 0; // Return 0 on success
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


int main(int argc, char *argv[]) {
    // Check for correct number of arguments
    if (argc < 4) {
        fprintf(stderr, "Usage: %s disk_path [FUSE options] mount_point\n", argv[0]);
        return EXIT_FAILURE;
    }

    disk_path = argv[1]; // Get the disk path from command-line arguments
    mount_point = argv[argc - 1]; // Get the mountpoint from command-line arguments

    // Initialize FUSE arguments
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    // Add FUSE options
    for (int i = 2; i < argc - 1; ++i) {
        fuse_opt_add_arg(&args, argv[i]);
    }

    // Mount the FUSE filesystem onto the specified mountpoint
    if (fuse_mount(mount_point, &args) == NULL) {
        perror("fuse_mount");
        fuse_opt_free_args(&args);
        return EXIT_FAILURE;
    }

    // Start the FUSE event loop with the provided callback functions
    int ret = fuse_main(args.argc, args.argv, &ops, NULL);

    // Unmount the filesystem and free memory
    fuse_unmount(mount_point, &args);
    fuse_opt_free_args(&args);

    return ret;
}