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
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Calculate the offset of the inode on disk
    off_t offset = sizeof(off_t) * 4 + (inode_index * sizeof(struct wfs_inode));

    // Move the file cursor to the inode offset
    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        return -1;
    }

    // Read the inode from disk
    ssize_t bytes_read = read(fd, inode, sizeof(struct wfs_inode));
    if (bytes_read != sizeof(struct wfs_inode)) {
        perror("read");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// static int write_inode(int inode_index, struct wfs_inode *inode) {
//     int fd = open(disk_path, O_RDWR);
//     if (fd == -1) {
//         perror("open");
//         return -1;
//     }

//     // Calculate the offset of the inode on disk
//     off_t offset = sizeof(off_t) * 4 + (inode_index * sizeof(struct wfs_inode));

//     // Move the file cursor to the inode offset
//     if (lseek(fd, offset, SEEK_SET) == -1) {
//         perror("lseek");
//         close(fd);
//         return -1;
//     }

//     // Write the inode to disk
//     ssize_t bytes_written = write(fd, inode, sizeof(struct wfs_inode));
//     if (bytes_written != sizeof(struct wfs_inode)) {
//         perror("write");
//         close(fd);
//         return -1;
//     }

//     close(fd);
//     return 0;
// }

// Function to get the inode index corresponding to a given path
static int get_inode_index(const char *path) {
    // Open the disk image in read mode
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Read the superblock to get the root inode index
    off_t superblock_offset = 0;

    // Read the superblock from disk
    struct wfs_sb superblock;
    if (pread(fd, &superblock, sizeof(struct wfs_sb), superblock_offset) != sizeof(struct wfs_sb)) {
        perror("pread");
        close(fd);
        return -1;
    }

    // Get the root directory inode
    struct wfs_inode root_inode;
    if (read_inode(superblock.num_inodes - 1, &root_inode) == -1) {
        close(fd);
        return -1;
    }

    // Close the disk image
    close(fd);

    // If the path is the root directory
    if (strcmp(path, "/") == 0)
        return superblock.num_inodes - 1;

    // For simplicity, we assume that the root directory contains direct entries only
    // Traverse the root directory entries to find the matching inode index
    // You would need to implement a more elaborate traversal for a real filesystem
    for (int i = 0; i < D_BLOCK; i++) {
        // Read the block from disk
        struct wfs_dentry directory_entry;
        if (pread(fd, &directory_entry, sizeof(struct wfs_dentry), root_inode.blocks[i]) != sizeof(struct wfs_dentry)) {
            perror("pread");
            close(fd);
            return -1;
        }

        // Check if the entry matches the path
        if (strcmp(directory_entry.name, path + 1) == 0) { // Skip the leading '/'
            return directory_entry.num;
        }
    }

    // If the path doesn't exist
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

    // Parse FUSE options
    if (fuse_opt_parse(&args, NULL, NULL, NULL) == -1) {
        return EXIT_FAILURE;
    }

//     // Mount the FUSE filesystem onto the specified mountpoint
    struct fuse_chan *channel = fuse_mount(mount_point, &args);
    if (channel == NULL) {
        perror("fuse_mount");
        fuse_opt_free_args(&args);
        return EXIT_FAILURE;
    }

//     // Start the FUSE event loop with the provided callback functions
   int ret = fuse_main(args.argc, args.argv, &ops, NULL);

//     // Unmount the filesystem and free memory
    fuse_unmount(mount_point, channel);
    fuse_opt_free_args(&args);

   return ret;
}

