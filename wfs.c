#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

char *disk_path;
char *mount_point;

static int wfs_getattr(const char *path, struct stat *stbuf)
{
    // Implementation of getattr function to retrieve file attributes
    // Fill stbuf structure with the attributes of the file/directory indicated by path
    // ...

    return 0; // Return 0 on success
}

static int wfs_mknod()
{
    return 0; // Return 0 on success
}

static int wfs_mkdir()
{
    return 0; // Return 0 on success
}

static int wfs_unlink()
{
    return 0; // Return 0 on success
}

static int wfs_rmdir()
{
    return 0; // Return 0 on success
}

static int wfs_read()
{
    return 0; // Return 0 on success
}

static int wfs_write()
{
    return 0; // Return 0 on success
}

static int wfs_readdir()
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