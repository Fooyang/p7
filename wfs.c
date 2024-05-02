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
int last_inode_num = 0;

void modify_inode_bitmap(int inode_num, int value) {
    int bitmap_index = inode_num / 8;
    int bitmap_bit = inode_num % 8;
    char* inode_bitmap = mem + sb->i_bitmap_ptr;
    if (value == 1) {
        // set bit
        inode_bitmap[bitmap_index] |= 1 << bitmap_bit;
    } else if (value == 0) {
        // clear bit
        inode_bitmap[bitmap_index] |= ~(1 << bitmap_bit);
    }
}

int is_inode_allocated(int inode_num) {
    int bitmap_index = inode_num / 8;
    int bitmap_bit = inode_num % 8;
    char* inode_bitmap = mem + sb->i_bitmap_ptr;
    return ((inode_bitmap[bitmap_index] >> bitmap_bit) & 1);
    
}

void modify_data_bitmap(int num, int value) {
    int bitmap_index = num / 8;
    int bitmap_bit = num % 8;
    char* data_bitmap = mem + sb->d_bitmap_ptr;
    if (value == 1) {
        // set bit
        data_bitmap[bitmap_index] |= 1 << bitmap_bit;
    } else if (value == 0) {
        // clear bit
        data_bitmap[bitmap_index] |= ~(1 << bitmap_bit);
    }
}

int is_data_block_allocated(int num) {
    int bitmap_index = num / 8;
    int bitmap_bit = num % 8;
    char* data_bitmap = mem + sb->i_bitmap_ptr;
    return ((data_bitmap[bitmap_index] >> bitmap_bit) & 1);
}

void print_inode_bitmap() {
    char* inode_bitmap = mem + sb->i_bitmap_ptr;
    printf("printing inode bitmap\n");
    for (int i = 0; i < sb->num_inodes; i++) {
        printf("%d ", is_inode_allocated(i));
    }
    printf("\n");
}

void print_data_bitmap() {
    char* data_bitmap = mem + sb->d_bitmap_ptr;
    printf("printing data bitmap\n");
    for (int i = 0; i < sb->num_data_blocks; i++) {
        printf("%d ", is_data_block_allocated(i));
    }
    printf("\n");
}

void write_to_inode(struct wfs_inode *inode) {
    struct wfs_inode *inode_position = (struct wfs_inode *) (mem + sb->i_blocks_ptr + inode->num * BLOCK_SIZE);
    memcpy(inode_position, inode, sizeof(inode));
}


static int read_inode(int inode_index, struct wfs_inode *inode)
{
    printf("entering read_inode\n");

    // Calculate the offset of the inode bitmap
    off_t inode_bitmap_offset = sb->i_bitmap_ptr + ((inode_index / 8));

    // Read the inode bitmap
    // char inode_bitmap = mem[inode_bitmap_offset];
    char *inode_bitmap_position = mem + inode_bitmap_offset;

    // Check if the inode index is valid
    if (inode_index >= sb->num_inodes)
    {
        fprintf(stderr, "Invalid inode index\n");
        return -1;
    }

    // Check if the inode is allocated in the bitmap
    if (!(*inode_bitmap_position & (1 << inode_index % 8)))
    {
        // Inode not allocated
        printf("inode at inode index %d is not allocated\n", inode_index);
        return -1;
    }

    // // Calculate the offset of the inode on disk
    off_t inode_offset = sb->i_blocks_ptr + (inode_index * BLOCK_SIZE);

    // Read the inode from the disk image
    memcpy(inode, mem + inode_offset, sizeof(struct wfs_inode));
    printf("going to exit read_inode\n");

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
        memcpy(&inode, mem + sb->i_blocks_ptr + (inode_index * BLOCK_SIZE), sizeof(struct wfs_inode));

        // Search for the token in the directory entries
        int found = 0;
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (inode.blocks[i] == 0)
            {
                break; // No more blocks
            }

            // Calculate the pointer to the block
            struct wfs_dentry *block = (struct wfs_dentry *)((uintptr_t)mem + inode.blocks[i]);

            // Iterate over the dentries in the block
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                struct wfs_dentry directory_entry = block[j];

                // Compare the directory entry name with the token
                if (strcmp(directory_entry.name, token) == 0)
                {
                    inode_index = directory_entry.num; // Update inode index
                    found = 1;
                    break;
                }
            }

            if (found)
            {
                break; // Break the outer loop if the directory entry was found
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
    printf("entering wfs_getattr\n");
    // Initialize the struct stat with 0s
    memset(stbuf, 0, sizeof(struct stat));

    // Get the inode index corresponding to the path
    int inode_index = get_inode_index(path);

    if (inode_index == -1)
    {
        // Path doesn't exist
        printf("%s\n", path);
        printf("NO PATH IN LS!\n");
        return -ENOENT;
    }

    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        return -EIO;
    }
    printf("in get attr, just after read_inode");

    stbuf->st_uid = inode.uid;      // Owner user ID
    stbuf->st_gid = inode.gid;      // Owner group ID
    stbuf->st_mtime = inode.mtim;   // Last modification time
    stbuf->st_mode = inode.mode;    // Regular file with permissions
    stbuf->st_nlink = inode.nlinks; // Number of hard links
    stbuf->st_size = inode.size;    // File size in bytes


    printf("nlinks is what ? %d\n", (int) stbuf->st_nlink);
    printf("in get attr, after putting things in the buffer\n");

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
    printf("exiting extract filename\n");
}

static int allocate_data_block()
{
    int data_bitmap_block_found = 0;
    uint8_t *ptr = NULL;
    int data_bitmap_index = 0;
    for (ptr = (uint8_t *)sb->d_bitmap_ptr; ptr < (uint8_t *)sb->i_blocks_ptr; ptr++)
    {
        for (int j = 0; j < 8; j++)
        {
            data_bitmap_index += 1;
            if (!(*ptr & (1 << j)))
            {
                printf("boutta set the %dth bit to 1\n", j);
                // Set the jth bit to 1
                *ptr |= (1 << j);
                data_bitmap_block_found = 1;
                break;
                // Exit the function after setting the bit
            }
        }
        if (data_bitmap_block_found == 1)
        {
            break;
        }
    }
    return data_bitmap_index;
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
    printf("inode number we are getting from get inode index in update: %d\n", inode_index);
    if (read_inode(inode_index, &inode) == -1)
    {
        printf("read inode returned -1\n");
        return -EIO;
    }
    printf("out of read inode\n");

    printf("in update\n");
    printf("inode : %d\n", inode.num);
    printf("inode: %d\n", inode.nlinks);

    // need to add new directory_entry to the end of the directory entries
    // if offset is not specified
    int found = 0;

    if (offset < 0)
    {
        // if the first offset is 0, we need to allocate a new data block
        if (inode.blocks[0] == 0)
        {
            printf("going to allocate a new data block\n");
            int data_index = allocate_data_block();
            inode.blocks[0] = (off_t)((char *)sb->d_blocks_ptr + data_index * BLOCK_SIZE);
            char *new_address = (char *)mem + inode.blocks[0];
            memcpy(new_address, &data, sizeof(off_t));
        }
        else
        {
            // int found = 0;
            for (int i = 0; i < N_BLOCKS; i++)
            {
                if (inode.blocks[i] == 0)
                {
                    char *new_address = (char *)mem + inode.blocks[i];
                    memcpy(new_address, &data, sizeof(off_t));
                    found = 1;
                }
            }
            // might need to code an indirect block for this
            if (found == 0)
            {
                printf(" in indirect block territory \n");
                // find block and set in data bitmap
                // int data_bitmap_index = allocate_data_block();
            }
        }
    }
    else
    {
        if (inode.blocks[offset] == 0)
        {
            memcpy(&(inode.blocks[offset]), &data, sizeof(off_t));
            found = 1;
        }
    }

    printf("getting to the end of update\n");

    return 0;
}

static int allocate_inode(const char *path, mode_t mode)
{

    time_t t;
    time(&t);

    last_inode_num++;

    // set inode in inode block
    struct wfs_inode *inode_ptr = (struct wfs_inode *)((char *)mem + sb->i_blocks_ptr + last_inode_num);
    inode_ptr->num = last_inode_num;
    inode_ptr->mode = mode;
    inode_ptr->uid = getuid();
    inode_ptr->gid = getgid();
    inode_ptr->size = 0;
    inode_ptr->nlinks = 1;
    inode_ptr->atim = t;
    inode_ptr->mtim = t;
    inode_ptr->ctim = t;
    memset(inode_ptr->blocks, 0, sizeof(inode_ptr->blocks));

    printf("inode has been set in inode block\n");

    // set inode in bitmap
    off_t inode_bitmap_offset = sb->i_bitmap_ptr + (last_inode_num / 8);
    char *inode_bitmap_position = mem + inode_bitmap_offset;

    // Check if the inode index is valid
    if (last_inode_num >= sb->num_inodes)
    {
        fprintf(stderr, "Invalid inode index\n");
        return -1;
    }

    // allocate inode in bitmap
    *inode_bitmap_position |= (1 << (last_inode_num % 8));
    printf("inode allocated in bitmap\n");

    return 0;
}


static int make(const char *path, mode_t mode)
{

    // allocate new inode
    // update parent inode
    // can mean adding a new data block if necessary

    printf("here is the path %s\n", path);
    // check if path is already present (it shouldn't be)
    if (get_inode_index(path) == 0)
    {
        printf("the new inode already exists\n");
        return -1;
    }

    char *mutable_path = strdup(path);
    char name[128];
    extract_filename(mutable_path, name);

    struct wfs_dentry dentry;
    strcpy(dentry.name, name);
    dentry.num = last_inode_num + 1;
    printf("the dentry that we are storing in the parent is : %s, %d\n", dentry.name, dentry.num);

    char *parent_path = dirname(mutable_path);
    printf("parent path %s\n", parent_path);
    // modify parent directory, then create new inode
    if (update(parent_path, &dentry, -1) < 0)
    {
        return -1;
    }
    printf("update done\n");

    return allocate_inode(path, mode);
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    // printf("entering wfs_mknod\n");
    mode |= __S_IFREG;
    return make(path, mode);

}

static int wfs_mkdir(const char *path, mode_t mode)
{
    // printf("entering wfs_mkdir\n");
    mode |= __S_IFDIR
    return make(path, mode); // Return 0 on success

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

    printf("%s IN WFS_READ", path);
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

    // Check if the inode represents a directory
    if (S_ISDIR(inode.mode))
    {
        return -EISDIR; // Not a regular file
    }

    // Calculate remaining size
    int remaining = inode.size - offset;
    if (remaining <= 0)
    {
        return 0; // No more data to read
    }

    // Determine the size to read
    size_t read_size = remaining >= size ? size : remaining;

    // Read file contents
    ssize_t bytes_read = 0;
    for (int i = 0; i < N_BLOCKS && bytes_read < read_size; i++)
    {
        if (inode.blocks[i] == 0)
        {
            break; // No more blocks
        }

        // Calculate the pointer to the block
        char *block = mem + inode.blocks[i];

        // Calculate the size to read from this block
        size_t block_size = read_size - bytes_read;
        if (block_size > BLOCK_SIZE)
        {
            block_size = BLOCK_SIZE;
        }

        // Copy data from the block to the buffer
        memcpy(buf + bytes_read, block, block_size);
        bytes_read += block_size;
    }

    return bytes_read; // Return the number of bytes read

}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    // int status = update(path, (void*)buf, offset);
    // if (status != 0) {
    //     return status;
    // }
    // return size;
    return 0;
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

    // Iterate over the blocks in the inode
    for (int i = 0; i < N_BLOCKS; i++)
    {
        // If the block pointer is 0, then there are no more blocks
        if (inode.blocks[i] == 0)
        {
            break;
        }

        // Calculate the pointer to the block
        struct wfs_dentry *block = (struct wfs_dentry *)((uintptr_t)mem + inode.blocks[i]);

        // Iterate over the dentries in the block
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            // If the dentry name is empty, then there are no more dentries
            if (block[j].name[0] == '\0')
            {
                break;
            }

            // Add the dentry to the filler buffer
            if (filler(buf, block[j].name, NULL, 0) != 0)
            {
                return -ENOMEM; // Buffer full
            }
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
    root_inode = (struct wfs_inode *)((char *)mem + sb->i_blocks_ptr);
    printf("the nlinks of the rootinode at the start are : %d\n", root_inode->nlinks);
    printf("the superblock is %d\n", (int) sb->num_data_blocks);
    printf("the root inode is %d\n", (int) root_inode->size);

    //     // Start the FUSE event loop with the provided callback functions
    int ret = fuse_main(argc - 1, new_args, &ops, NULL);

    return ret;
}