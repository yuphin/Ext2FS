#include <fcntl.h>
#include <cstdlib>
#include <unistd.h>
#include "ext2.h"
#include <iostream>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <math.h>
#include <bitset>
#define BASE_OFFSET 1024
#define PROT_WR (uint32_t)PROT_READ|(uint32_t)PROT_WRITE


char* load_image(char * path){
    int fd = open(path, O_RDWR);
    struct stat sb{};
    if(fd == -1){
        std::perror("Error opening image file");
        exit(EXIT_FAILURE);
    }
    if (fstat(fd, &sb) == -1)           /* To obtain file size */
        std::perror("Cannot read stats");
    auto* img = static_cast<char*> (mmap(nullptr, sb.st_size, PROT_WR, MAP_SHARED, fd, 0));
    if (img == MAP_FAILED)
        std::perror("Failed to initialize mmap");
    close(fd);
    return img;

}


struct Bitmap{
    Bitmap(char* bmp){
        this->bmp = bmp;

    }
    void print_byte_at_idx(unsigned int idx){
        std::cout << std::bitset<8>(bmp[idx]) << std::endl;
    }
    void set(uint8_t idx){
        bmp[idx / 8u] |= 1u << (idx % 8u);
    }
    void clear(uint8_t idx){
        bmp[idx/8u] &= ~(1u << (idx % 8u));
    }
    bool is_set(uint8_t idx){
        return bmp[idx/8u] & (1u <<  (idx % 8u));
    }
    char *bmp;
};


struct Filesystem{
    ext2_super_block* sup;
    char *img;
    unsigned int blksz;
    unsigned int ninode;
    unsigned int nblk;
    unsigned int inosz;
    unsigned int inopgrp;
    unsigned int blkpgrp;
    unsigned int ngroups;
    unsigned int inode_bitmap_size;
    unsigned int block_bitmap_size;
    ext2_group_desc* gdt;

    explicit Filesystem(char* img){
        this->img = img;
        sup = reinterpret_cast<ext2_super_block*>(get_block(1));
        blksz = static_cast<unsigned int>(pow(2,10+sup->s_log_block_size));
        ninode = sup->s_inodes_count;
        nblk = sup->s_blocks_count;
        inosz = sup->s_inode_size;
        inopgrp = sup->s_inodes_per_group;
        blkpgrp = sup->s_blocks_per_group;
        ngroups = ceil(this->ninode/inopgrp);
        auto block_no = (uint32_t)ceil(((float)BASE_OFFSET+ sizeof(sup))/ blksz);
        gdt = reinterpret_cast<ext2_group_desc*>(get_block(block_no));
        inode_bitmap_size =  inopgrp / 8;
        block_bitmap_size = blkpgrp / 8;
        auto* fst = get_first_datablk(0);

    }
    char *get_first_datablk(unsigned int group_no){
        auto block_no = gdt[group_no].bg_inode_table + (uint32_t)ceil( ((float)inosz + inopgrp)/blksz);
        return get_block(block_no);
    }
    char* get_block(unsigned int block_id){
        return  img + BASE_OFFSET + (block_id-1) * this->blksz;
    }

    Bitmap get_bat(unsigned int group_no){
        auto block_no = gdt[group_no].bg_block_bitmap;
        return {get_block(block_no)};
    }
    Bitmap get_iat(unsigned int group_no){
        auto block_no = gdt[group_no].bg_inode_bitmap;
        return {get_block(block_no)};

    }
    ext2_inode* get_inode_table(unsigned int group_no){
        auto block_no = gdt[group_no].bg_inode_table;
        return reinterpret_cast<ext2_inode*>(get_block(block_no));
    }


};



int main(int argc,char *argv[]) {
    auto * img = load_image(argv[1]);
    auto fs = Filesystem(img);
    
    return 0;
}