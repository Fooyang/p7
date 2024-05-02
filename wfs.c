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
    printf("printing inode bitmap\n");
    for (int i = 0; i < sb->num_inodes; i++) {
        printf("%d ", is_inode_allocated(i));
    }
    printf("\n");
}

void print_data_bitmap() {
    printf("printing data bitmap\n");
    for (int i = 0; i < sb->num_data_blocks; i++) {
        printf("%d ", is_data_block_allocated(i));
    }
    printf("\n");
}

void print_inode_offsets() {
    printf("printing inode offsets and dentries\n");
    for (int i = 0; i < sb->num_inodes; i++) {
        if (is_inode_allocated(i) == 1) {
            struct wfs_inode *inode = (struct wfs_inode *) (mem + sb->i_blocks_ptr + i * BLOCK_SIZE);
            printf("inode num: %d ", inode->num);
            for (int j = 0; j < N_BLOCKS; j++) {
                printf("offset %d: %d dentry name: %s", j, (int) inode->blocks[j],
                 ((struct wfs_dentry *)(mem + inode->blocks[j]))->name);
            }
            printf("\n");
        }
    }
}

void write_to_inode(struct wfs_inode *inode) {
    struct wfs_inode *inode_position = (struct wfs_inode *) (mem + sb->i_blocks_ptr + inode->num * BLOCK_SIZE);
    memcpy(inode_position, inode, sizeof(struct wfs_inode));
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
    int off = inode_index * BLOCK_SIZE;
    off_t inode_offset = sb->i_blocks_ptr;
    inode_offset = inode_offset + off;

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

// Function to get attributes of a file or directory
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

    printf("%d inode index", inode_index);
    // Read the inode from disk
    struct wfs_inode inode;
    if (read_inode(inode_index, &inode) == -1)
    {
        return -EIO;
    }

    printf("%d from inode", inode.num);
    printf("in get attr, just after read_inode");

    stbuf->st_dev = 0;
    stbuf->st_ino = inode.num;
    stbuf->st_mode = inode.mode;
    stbuf->st_uid = inode.uid;
    stbuf->st_gid = inode.gid;
    stbuf->st_size = inode.size;
    stbuf->st_blksize = BLOCK_SIZE;
    stbuf->st_blocks = inode.size / BLOCK_SIZE;
    stbuf->st_atime = inode.atim;
    stbuf->st_mtime = inode.mtim;
    stbuf->st_ctime = inode.ctim;


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
    int index;
    for (index = 0; index < sb->num_data_blocks; index++) {
        if (is_data_block_allocated(index) == 0) {
            modify_data_bitmap(index, 1);
            break;
        }
    }
    return index;
}

void add_dentry(struct wfs_inode *inode, struct wfs_dentry *dentry) {

    int data_block_num = 0;
    // if empty, allocate new data block

    for (int i = 0; i <= D_BLOCK; i++) {
        if (inode->blocks[i] == 0) {
            data_block_num = allocate_data_block();
            char* dentry_destination = mem + sb->d_blocks_ptr + data_block_num * BLOCK_SIZE;
            memcpy(dentry_destination, dentry, sizeof(struct wfs_dentry));
            off_t offset_to_store = (off_t) (dentry_destination - mem);
            printf("setting offset of dentry to %d in inode %d and i is %d\n", (int) offset_to_store, inode->num, i);
            // inode->blocks[i] = offset_to_store;
            struct wfs_inode *new_inode = (struct wfs_inode *) (mem + sb->i_blocks_ptr + inode->num * BLOCK_SIZE);
            new_inode->blocks[i] = offset_to_store;
            printf("the new inode offset 0 is %d\n", (int) new_inode->blocks[0]);
            break;
        }
    }
}

int find_dentry(char *name, off_t *blocks) {
    printf("in find dentry\n");

    for (int i = 0; i < N_BLOCKS; i++) {
        printf("entering loop on iteration %d\n", i);
        if (blocks[i] == 0) {
            printf("blocks i is 0\n");
            continue;
        }
        struct wfs_dentry *dentry_location = (struct wfs_dentry *) (mem + blocks[i]);
        if (strncmp(name, dentry_location->name, strlen(name)) == 0) {
            printf("exiting find dentry\n");
            return 0;
        }

    }
    printf("exiting find dentry\n");
    return -1;
}


static int update_parent(char *path, struct wfs_dentry *dentry, int offset)
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
        printf("read inode returned -1\n");
        return -EIO;
    }
    // check if dentry already stored
    if (find_dentry(dentry->name, inode.blocks) == 0) {
        printf("dentry already in parent");
        return -EEXIST;
    }

    add_dentry(&inode, dentry);
    return 0;
}

static int allocate_inode(const char *path, mode_t mode)
{

    time_t t;
    time(&t);

    last_inode_num++;

    // set inode in inode block
    struct wfs_inode inode_ptr = {0};
    inode_ptr.num = last_inode_num;
    inode_ptr.mode = mode;
    inode_ptr.uid = getuid();
    inode_ptr.gid = getgid();
    inode_ptr.size = 0;
    inode_ptr.nlinks = 1;
    inode_ptr.atim = t;
    inode_ptr.mtim = t;
    inode_ptr.ctim = t;
    inode_ptr.blocks[0] = 0;
    write_to_inode(&inode_ptr);

    printf("inode has been set in inode block\n");

    // set inode in bitmap
    // off_t inode_bitmap_offset = sb->i_bitmap_ptr + (last_inode_num / 8);
    // char *inode_bitmap_position = mem + inode_bitmap_offset;

    // Check if the inode index is valid
    if (last_inode_num >= sb->num_inodes)
    {
        fprintf(stderr, "Invalid inode index\n");
        return -1;
    }

    // allocate inode in bitmap
    modify_inode_bitmap(last_inode_num, 1);
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
    if (update_parent(parent_path, &dentry, -1) < 0)
    {
        return -1;
    }

    print_data_bitmap();
    print_inode_offsets();
    printf("update done\n");

    if (allocate_inode(path, mode) < 0) {
        return -1;
    }
    print_inode_bitmap();
    return 0;
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
    mode |= __S_IFDIR;
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

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
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
    char *new_args[argc];
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
    fd = open(disk_path, O_RDWR, 0666);
    file_size = lseek(fd, 0, SEEK_END);
    mem = mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    sb = (struct wfs_sb *)mem;
    root_inode = (struct wfs_inode *)(mem + sb->i_blocks_ptr);
    printf("the nlinks of the rootinode at the start are : %d\n", root_inode->nlinks);
    printf("the superblock is %d\n", (int) sb->num_data_blocks);
    printf("the root inode is %d\n", (int) root_inode->size);

    //     // Start the FUSE event loop with the provided callback functions
    int ret = fuse_main(argc - 1, new_args, &ops, NULL);

    munmap(mem, file_size);
    close(fd);
    
    return ret;
}