#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include "wfs.h"
#include <linux/stat.h>
#include <stdint.h>

int roundup(int num, int factor) {
    return num % factor == 0 ? num : num + (factor - (num % factor));
}

int create_superblock(void *addr, int num_inodes, int num_data_blocks) {
    struct wfs_sb *superblock = (struct wfs_sb *)addr;
    superblock->num_inodes = num_inodes;
    superblock->num_data_blocks = num_data_blocks;
    superblock->i_bitmap_ptr = sizeof(struct wfs_sb); // Start of inode bitmap
    superblock->d_bitmap_ptr = superblock->i_bitmap_ptr + (num_inodes / 8); // Start of data bitmap
    superblock->i_blocks_ptr = superblock->d_bitmap_ptr + (num_data_blocks / 8); // Start of inode blocks
    superblock->d_blocks_ptr = superblock->i_blocks_ptr + (BLOCK_SIZE * num_inodes); // Start of data blocks

//    uint8_t *inode_bitmap_ptr = (uint8_t *)addr + sizeof(struct wfs_sb);
//     *inode_bitmap_ptr = 8; // Set the first bit to 1


    return 0;
}

int create_root_inode(void *addr, int num_inodes) {
    struct wfs_inode *root_inode = (struct wfs_inode *)((char *)addr + sizeof(struct wfs_sb));
    memset(root_inode, 0, sizeof(struct wfs_inode)); // Initialize all fields to 0
    root_inode->uid = getuid(); // Owner user ID
    root_inode->gid = getgid(); // Owner group ID
    root_inode->nlinks = 1; // Number of directory links (initially empty)


    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 7)
    {
        printf("Correct amount of arguments has not been passed in\n");
        return EXIT_FAILURE;
    }
    int num_inodes;
    int num_data_blocks;
    char *filename;
    int op;
   
    while ((op = getopt(argc, argv, "d:i:b:")) != -1)
    {
        switch (op)
        {
        case 'd':
            filename = optarg;
            break;
        case 'i':
            num_inodes = atoi(optarg);
            num_inodes = roundup(num_inodes, 32);
            break;
        case 'b':
            num_data_blocks = atoi(optarg);
            // round the number of blocks up to the nearest multiple of 24
            num_data_blocks = roundup(num_data_blocks, 32);
            break;
        }
    }
    // checking if all flags fulfill
    if (filename == NULL || num_inodes <= 0 || num_data_blocks <= 0)
    {
        fprintf(stderr, "Error: Insufficient or invalid arguments\n");
        return EXIT_FAILURE;
    }

    // opening the file for the file system
    int fd = open(filename, O_RDWR);
    if (fd == -1)
    {
        perror("Error opening file");
        return EXIT_FAILURE;
    }



    struct stat file_info;
    if (fstat(fd, &file_info) == -1)
    {
        perror("fstat");
        close(fd);
        exit(EXIT_FAILURE);
    }

    size_t total_size_required = sizeof(struct wfs_sb) + (num_inodes / 8) +
                                 (num_data_blocks / 8) + sizeof(struct wfs_inode) * num_inodes +
                                 + BLOCK_SIZE * num_data_blocks;

    // Round up total size to the nearest multiple of 32
    total_size_required = roundup(total_size_required, 32);

    // not enough space in the file
    if (file_info.st_size < total_size_required) {
        close(fd);
        return -1;
    }



    // Memory map the file
    void *addr = mmap(NULL, total_size_required, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    if (create_superblock(addr, num_inodes, num_data_blocks) == -1) {
        close(fd);
        munmap(addr, total_size_required);
        return EXIT_FAILURE;
    }

    if (create_root_inode(addr, num_inodes) == -1) {
        close(fd);
        munmap(addr, total_size_required);
        return EXIT_FAILURE;
    }

    // Unmap the memory-mapped file
    munmap(addr, total_size_required);

    close(fd);

    return EXIT_SUCCESS;
}