#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <libgen.h>
#include <stddef.h>

char *disk_path;
char *mount_point;
int file_size;
int fd;
char *mem;
struct wfs_sb *sb;
struct wfs_inode *root_inode;
int last_inode_num;

static int read_inode(int inode_index, struct wfs_inode *inode)
{
    // Calculate the offset of the inode bitmap
    off_t inode_bitmap_offset = sb->i_bitmap_ptr + ((inode_index / 8) * sizeof(char));

    // Read the inode bitmap
    char inode_bitmap = mem[inode_bitmap_offset];

    // Check if the inode index is valid
    if (inode_index >= sb->num_inodes)
    {
        fprintf(stderr, "Invalid inode index\n");
        return -1;
    }

    // Check if the inode is allocated in the bitmap
    if (!(inode_bitmap & (1 << (inode_index % 8))))
    {
        // Inode not allocated
        return -1;
    }

    // Calculate the offset of the inode on disk
    off_t inode_offset = sb->i_blocks_ptr + (inode_index * sizeof(struct wfs_inode));

    // Read the inode from the disk image
    memcpy(inode, mem + inode_offset, sizeof(struct wfs_inode));

    return 0;
}

static int get_inode_index(const char *path)
{
    char *mutable_path = strdup(path); // Make a mutable copy of the path
    if (mutable_path == NULL)
    {
        perror("strdup");
        return -1;
    }

    // Initialize inode index with the root inode index
    int inode_index = 0;

    // Parse the path and traverse through the directories
    char *token = strtok(mutable_path, "/");
    while (token != NULL)
    {
        // Read the inode corresponding to the current index
        struct wfs_inode inode;
        memcpy(&inode, mem + sb->i_blocks_ptr + inode_index * sizeof(struct wfs_inode), sizeof(struct wfs_inode));

        // Search for the token in the directory entries
        int found = 0;
        for (int i = 0; i < D_BLOCK; i++)
        {
            struct wfs_dentry directory_entry;
            memcpy(&directory_entry, mem + sb->d_blocks_ptr + inode.blocks[i] * sizeof(struct wfs_dentry), sizeof(struct wfs_dentry));

            // Compare the directory entry name with the token
            if (strcmp(directory_entry.name, token) == 0)
            {
                inode_index = directory_entry.num; // Update inode index
                found = 1;
                break;
            }
        }

        // If the directory entry was not found
        if (!found)
        {
            fprintf(stderr, "Directory not found: %s\n", token);
            free(mutable_path); // Free the allocated memory
            return -1;
        }

        // Get the next token
        token = strtok(NULL, "/");
    }

    free(mutable_path); // Free the allocated memory
    return inode_index;
}

// // Function to get attributes of a file or directory
static int wfs_getattr(const char *path, struct stat *stbuf)
{
    // Initialize the struct stat with 0s
    memset(stbuf, 0, sizeof(struct stat));

    // Get the inode index corresponding to the path
    int inode_index = get_inode_index(path);
    if (inode_index == -1)
    {
        // Path doesn't exist
        return -ENOENT;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        return -EIO;
    }

    stbuf->st_uid = inode.uid;      // Owner user ID
    stbuf->st_gid = inode.gid;      // Owner group ID
    stbuf->st_atime = inode.atim;   // Last access time
    stbuf->st_mtime = inode.mtim;   // Last modification time
    stbuf->st_mode = inode.mode;    // Regular file with permissions
    stbuf->st_nlink = inode.nlinks; // Number of hard links
    stbuf->st_size = inode.size;    // File size in bytes

    return 0;
}

void extract_filename(const char *path, char *filename)
{
    const char *last_slash = strrchr(path, '/');

    // If '/' is found, copy the substring after it to filename
    if (last_slash != NULL)
    {
        strcpy(filename, last_slash + 1);
    }
    else
    {
        // If no '/', the path itself is the filename
        strcpy(filename, path);
    }
}

static int update(char *path, void *data, int offset)
{

    int inode_index = get_inode_index(path);
    if (inode_index == -1)
    {
        // Path doesn't exist
        return -ENOENT;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        return -EIO;
    }

    // need to add new directory_entry to the end of the directory entries
    // if offset is not specified

    if (offset < 0)
    {
        // int found = 0;
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (inode.blocks[i] == 0)
            {
                memcpy(&(inode.blocks[i]), &data, sizeof(off_t));
                // found = 1;
            }
        }
    }
    else
    {
        memcpy(&(inode.blocks[offset]), &data, sizeof(off_t));
    }

    return 0;
}

static int add(const char *path, mode_t mode)
{

    time_t t;
    time(&t);
    last_inode_num++;
    off_t blocks[N_BLOCKS]; // Define the blocks array

    // Initialize blocks and set all elements to 0
    memset(blocks, 0, sizeof(blocks));
    char *mutable_path = strdup(path);
    char *filename = NULL;
    struct wfs_dentry dentry_1;
    struct wfs_dentry dentry_2;
    extract_filename(mutable_path, filename);
    char *parent_path = dirname(mutable_path);
    dentry_1.num = get_inode_index(path);
    dentry_2.num = get_inode_index(parent_path);
    strcpy(dentry_1.name, ".");
    strcpy(dentry_2.name, "..");



    // find block and set in data bitmap
    int found = 0;
    uint8_t *ptr = NULL;
    int offset = 0;
    for (ptr = (uint8_t *)sb->d_bitmap_ptr; ptr < (uint8_t *)sb->i_blocks_ptr; ptr++)
    {
        for (int j = 0; j < 8; j++)
        {
            offset += 1;
            if (!(*ptr & (1 << j)))
            {
                // Set the jth bit to 1
                *ptr |= (1 << j);
                found = 1;
                break;
                // Exit the function after setting the bit
            }
        }
        if (found == 1)
        {
            break;
        }
    }


    // store the dentries in the data block
    struct wfs_dentry *block_ptr = (struct wfs_dentry *)((char *)sb->d_blocks_ptr + (offset * BLOCK_SIZE));
    *block_ptr = dentry_1;
    *(block_ptr + 1) = dentry_2;

    // struct wfs_inode inode =
    //     {
    //         .num = last_inode_num,
    //         .mode = mode,
    //         .uid = getuid(),
    //         .gid = getgid(),
    //         .size = 0,
    //         .nlinks = 1,
    //         .atim = t,
    //         .mtim = t,
    //         .ctim = t,
    //         .blocks = {blocks}};

    // // set in inode bitmap
    // uint8_t *ptr_2 = NULL;
    // for (ptr_2 = (uint8_t *)sb->i_bitmap_ptr; ptr_2 < (uint8_t *)sb->d_bitmap_ptr; ptr_2++)
    // {
    //     for (int j = 0; j < 8; j++)
    //     {
    //         if (!(*ptr_2 & (1 << j)))
    //         {
    //             // Set the jth bit to 1
    //             *ptr_2 |= (1 << j);
    //             found = 1;
    //             break;

    //             // Exit the function after setting the bit
    //         }
    //     }
    //     if (found == 1)
    //     {
    //         break;
    //     }
    // }

    // store inode in its block
    struct wfs_inode *inode_ptr = (struct wfs_inode *)((char *)sb->i_blocks_ptr + get_inode_index(path));
    inode_ptr->num = last_inode_num;
    inode_ptr->mode = mode;
    inode_ptr->uid = getuid();
    inode_ptr->gid = getgid();
    inode_ptr->size = 0;
    inode_ptr->nlinks = 1;
    inode_ptr->atim = t;
    inode_ptr->mtim = t;
    inode_ptr->ctim = t;
    inode_ptr->blocks[0] = (off_t)((char *)sb->d_blocks_ptr + (offset * BLOCK_SIZE));
    inode_ptr->blocks[1] = inode_ptr->blocks[0] + sizeof(struct wfs_dentry);

        //         ,
    // //         .,
    // //         .uid = getuid(),
    // //         .gid = getgid(),
    // //         .size = 0,
    // //         .nlinks = 1,
    // //         .atim = t,
    // //         .mtim = t,
    // //         .ctim = t,
    // //         .blocks = {blocks}};



    return 0;
}

static int make(const char *path, mode_t mode)
{
    printf("printing!\n");
    // check if path is already present (it shouldn't be)
    if (get_inode_index(path) != 0)
    {
        printf("returning -1 because it wasn't found\n");
        return -1;
    }

    char *mutable_path = strdup(path);
    char *filename = NULL;
    extract_filename(mutable_path, filename);

    struct wfs_dentry dentry;
    strcpy(dentry.name, filename);
    dentry.num = last_inode_num + 1;

    char *parent_path = dirname(mutable_path);
    // modify parent directory, then create new inode
    if (update(parent_path, &dentry, -1) < 0)
    {
        return -1;
    }

    return add(path, mode);
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    return make(path, __S_IFREG);

}

static int wfs_mkdir(const char *path, mode_t mode)
{

    return make(path, __S_IFDIR); // Return 0 on success
}

static int wfs_unlink(const char *path)
{
    return 0; // Return 0 on success
}

static int wfs_rmdir(const char *path)
{
    return 0; // Return 0 on success
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    // Get the inode index for the given path
    int inode_index = get_inode_index(path);
    if (inode_index == -1)
    {
        // Path doesn't exist
        return -ENOENT;
    }

    // Open the disk image in read mode
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1)
    {
        perror("open");
        return -EIO;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        close(fd); // Close the file descriptor before returning
        return -EIO;
    }

    // Check if the inode represents a directory
    if (S_ISDIR(inode.mode))
    {
        close(fd);      // Close the file descriptor before returning
        return -EISDIR; // Not a regular file
    }

    // Read file contents
    ssize_t bytes_read = 0;
    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (inode.blocks[i] == 0)
        {
            break; // No more blocks
        }
        // Read block contents
        ssize_t block_size = (offset < inode.size) ? ((inode.size - offset < BLOCK_SIZE) ? inode.size - offset : BLOCK_SIZE) : 0;
        ssize_t bytes = pread(fd, buf + bytes_read, block_size, inode.blocks[i] * BLOCK_SIZE + offset);
        if (bytes == -1)
        {
            perror("pread");
            close(fd); // Close the file descriptor before returning
            return -EIO;
        }
        bytes_read += bytes;
        if (bytes < BLOCK_SIZE)
        {
            break; // Reached end of file
        }
    }

    close(fd); // Close the file descriptor before returning
    return bytes_read;

}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
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

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    // Get the inode index for the given directory path
    int inode_index = get_inode_index(path);
    if (inode_index == -1)
    {
        return -ENOENT; // Directory not found
    }

    // Read the inode information
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        return -EIO; // I/O error
    }

    // Check if the inode represents a directory
    if (!(inode.mode & S_IFDIR))
    {
        return -ENOTDIR; // Not a directory
    }

    // Get the directory entries and the count of dentries
    struct wfs_dentry *dentries = (struct wfs_dentry *)((uintptr_t)disk_path + (uintptr_t)sb->d_blocks_ptr + inode.blocks[0] * sizeof(struct wfs_dentry));
    int dentry_count = inode.size / sizeof(struct wfs_dentry);

    // Fill the buffer with directory entries
    for (int i = 0; i < dentry_count; i++)
    {
        // Add the dentry to the filler buffer
        if (filler(buf, dentries[i].name, NULL, 0) != 0)
        {
            return -ENOMEM; // Buffer full
        }
    }

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
    char *new_args[argc - 1];
    int new_count = 0;
    for (int i = 0; i < argc; i++)
    {
        if (i == 1)
        {
            continue;
        }
        new_args[new_count] = argv[i];
        new_count++;
    }
    new_args[new_count] = NULL;

    // mmap
    fd = open(disk_path, O_RDWR);
    file_size = lseek(fd, 0, SEEK_END);
    mem = mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    sb = (struct wfs_sb *)mem;
    root_inode = (struct wfs_inode *)((char *)mem + sizeof(struct wfs_sb));

    //     // Start the FUSE event loop with the provided callback functions
    int ret = fuse_main(argc - 1, new_args, &ops, NULL);

    return ret;
}