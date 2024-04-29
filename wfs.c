#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>

char *disk_path;
char *mount_point;

// Helper function to read an inode from disk
static int read_inode(int inode_num, struct wfs_inode *inode)
{
    int fd = open(disk_path, O_RDWR);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Calculate offset of inode on disk
    off_t offset = sizeof(struct wfs_sb) + (inode_num * sizeof(struct wfs_inode));
    
    // Seek to the position of the inode
    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        return -1;
    }

    // Read inode from disk
    if (read(fd, inode, sizeof(struct wfs_inode)) == -1) {
        perror("read");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int wfs_getattr(const char *path, struct stat *stbuf)
{
    // Initialize stbuf with default values
    memset(stbuf, 0, sizeof(struct stat));

    // Check if the path is the root directory
    if (strcmp(path, "/") == 0) {
        // Set attributes for the root directory
        stbuf->st_mode = S_IFDIR | 0755; // Directory with read, write, and execute permission for owner
        stbuf->st_nlink = 2; // Number of hard links (directories have at least 2: . and ..)
        return 0; // Return 0 on success
    }

    // Retrieve file attributes based on the given path
    struct wfs_inode inode;
    int inode_num = 0; // Assuming root directory inode number is 0
    char *token, *rest = strdup(path); // Duplicate path to avoid modifying original string
    int fd = open(disk_path, O_RDWR); // Open disk file
    if (fd == -1) {
        perror("open");
        free(rest);
        return -1;
    }

    while ((token = strsep(&rest, "/")) != NULL) {
        if (token[0] == '\0') // Skip empty tokens (e.g., consecutive slashes)
            continue;
        
        // Read directory inode from disk
        if (read_inode(inode_num, &inode) == -1) {
            close(fd);
            free(rest);
            return -errno;
        }

        // Look for the token (directory entry) in the directory's data blocks
        int found = 0;
        for (int i = 0; i < D_BLOCK; i++) {
            // Check if the data block is valid
            if (inode.blocks[i] == 0)
                break;

            // Read the data block from disk
            off_t offset = inode.blocks[i];
            if (lseek(fd, offset, SEEK_SET) == -1) {
                perror("lseek");
                close(fd);
                free(rest);
                return -1;
            }

            struct wfs_dentry dentry;
            if (read(fd, &dentry, sizeof(struct wfs_dentry)) == -1) {
                perror("read");
                close(fd);
                free(rest);
                return -1;
            }

            // Here you would compare dentry.name with token
            // If a match is found, update inode_num with the inode number of the directory entry
            if (strcmp(dentry.name, token) == 0) {
                inode_num = dentry.num; // Update inode number for next iteration
                found = 1;
                break;
            }
        }

        if (!found) {
            // Directory entry not found
            close(fd);
            free(rest);
            return -ENOENT;
        }
    }

    // Read final inode from disk
    if (read_inode(inode_num, &inode) == -1) {
        close(fd);
        free(rest);
        return -errno;
    }

    // Set attributes based on inode
    stbuf->st_mode = inode.mode; // File mode and permissions
    stbuf->st_uid = inode.uid; // Owner user ID
    stbuf->st_gid = inode.gid; // Owner group ID
    stbuf->st_nlink = inode.nlinks; // Number of hard links
    stbuf->st_size = inode.size; // Total size in bytes
    stbuf->st_atime = inode.atim; // Time of last access
    stbuf->st_mtime = inode.mtim; // Time of last modification
    stbuf->st_ctime = inode.ctim; // Time of last status change

    close(fd);
    free(rest);
    return 0; // Return 0 on success
}


static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
        // Check if the file already exists
    if (access(path, F_OK) == 0) {
        return -EEXIST; // Return "File exists" error
    }

    // Retrieve the directory path from the given path
    char dir_path[strlen(path) + 1];
    strcpy(dir_path, path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash == NULL) {
        return -EINVAL; // Return "Invalid argument" error
    }
    *last_slash = '\0'; // Terminate the directory path string

    // Open the directory
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return -ENOENT; // Return "No such file or directory" error
    }

    // Get the inode number of the parent directory
    struct stat st;
    if (stat(dir_path, &st) == -1) {
        closedir(dir);
        return -errno;
    }
    int parent_inode = st.st_ino;

    // Get the next available inode number for the new file
    int new_inode_num = get_next_available_inode();
    if (new_inode_num == -1) {
        closedir(dir);
        return -ENOSPC; // Return "No space left on device" error
    }

    // Create a new directory entry for the file
    struct wfs_dentry new_dentry;
    strcpy(new_dentry.name, last_slash + 1); // Copy the file name
    new_dentry.num = new_inode_num; // Set the inode number

    // Write the directory entry to the parent directory's data blocks
    if (write_dentry_to_dir(parent_inode, &new_dentry) == -1) {
        closedir(dir);
        return -errno;
    }

    // Create a new inode for the file
    struct wfs_inode new_inode;
    memset(&new_inode, 0, sizeof(struct wfs_inode));
    new_inode.mode = mode;
    new_inode.num = new_inode_num;
    new_inode.uid = getuid();
    new_inode.gid = getgid();

    // Write the new inode to disk
    if (write_inode_to_disk(new_inode_num, &new_inode) == -1) {
        closedir(dir);
        return -errno;
    }

    // Update the parent directory's modification time
    if (update_dir_modification_time(parent_inode) == -1) {
        closedir(dir);
        return -errno;
    }

    closedir(dir);
    return 0; // Return 0 on success
}

static int wfs_mkdir(const char* path, mode_t mode)
{
    // Check if the directory already exists
    if (access(path, F_OK) == 0) {
        return -EEXIST; // Return "File exists" error
    }

    // Retrieve the parent directory path and the name of the new directory
    char dir_path[strlen(path) + 1];
    strcpy(dir_path, path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash == NULL) {
        return -EINVAL; // Return "Invalid argument" error
    }
    *last_slash = '\0'; // Terminate the parent directory path string
    const char *new_dir_name = last_slash + 1; // Name of the new directory

    // Open the parent directory
    DIR *parent_dir = opendir(dir_path);
    if (parent_dir == NULL) {
        return -ENOENT; // Return "No such file or directory" error
    }

    // Check if the parent directory is writable
    if (access(dir_path, W_OK) != 0) {
        closedir(parent_dir);
        return -EACCES; // Return "Permission denied" error
    }

    // Get the inode number of the parent directory
    struct stat st;
    if (stat(dir_path, &st) == -1) {
        closedir(parent_dir);
        return -errno; // Return error
    }
    int parent_inode = st.st_ino;

    // Create a new inode for the directory
    int new_inode = get_next_available_inode(); // Function to get the next available inode
    if (new_inode == -1) {
        closedir(parent_dir);
        return -ENOSPC; // Return "No space left on device" error
    }

    // Set up the new inode
    struct wfs_inode dir_inode;
    memset(&dir_inode, 0, sizeof(struct wfs_inode));
    dir_inode.num = new_inode;
    dir_inode.mode = S_IFDIR | mode; // Set file type to directory and permissions
    dir_inode.uid = getuid(); // Set owner user ID
    dir_inode.gid = getgid(); // Set owner group ID
    dir_inode.nlinks = 2; // Set initial number of hard links (. and ..)
    dir_inode.size = 0; // Directory size is 0
    time(&dir_inode.atim); // Set access time to current time
    dir_inode.mtim = dir_inode.atim; // Set modification time to access time
    dir_inode.ctim = dir_inode.atim; // Set status change time to access time

    // Write the new inode to disk
    if (write_inode_to_disk(new_inode, &dir_inode) == -1) {
        closedir(parent_dir);
        return -errno; // Return error
    }

    // Update parent directory modification time
    if (update_dir_modification_time(parent_inode) == -1) {
        closedir(parent_dir);
        return -errno; // Return error
    }

    // Add new directory entry to the parent directory
    struct wfs_dentry new_dentry;
    memset(&new_dentry, 0, sizeof(struct wfs_dentry));
    strncpy(new_dentry.name, new_dir_name, MAX_NAME - 1); // Copy directory name
    new_dentry.num = new_inode; // Set inode number
    if (write_dentry_to_dir(parent_inode, &new_dentry) == -1) {
        closedir(parent_dir);
        return -errno; // Return error
    }

    closedir(parent_dir);
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

static int wfs_write(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
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
    if (fuse_mount(mount_point, &args) == -1) {
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