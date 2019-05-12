#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "ext2.h"
#include <iostream>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <math.h>
#include <bitset>
#include <tuple>
#include <dirent.h>

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
        for(int i=0; i< 1024;i++){
            std::cout << std::bitset<8>(bmp[i]) << std::endl;
        }

    }
    void set(uint32_t idx){
        bmp[idx / 8u] |= 1u << (idx % 8u);
    }
    void clear(uint32_t idx){
        bmp[idx/8u] &= ~(1u << (idx % 8u));
    }
    bool is_set(uint32_t idx){
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
        auto * root = get_inode(2);
        auto *dir = reinterpret_cast<ext2_dir_entry*>(get_block(9)+44);
        auto *inode = get_inode(12);
        inode_bitmap_size =  inopgrp / 8;
        block_bitmap_size = blkpgrp / 8;
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
    uint32_t get_first_datablk(unsigned int group_no){
        auto block_no = gdt[group_no].bg_inode_table + (uint32_t)ceil( ((float)inosz * inopgrp)/blksz);
        return block_no;
    }

    ext2_inode* get_inode(uint32_t i){
        uint32_t g = floor((float)(i-1)/ inopgrp);
        uint32_t j = (i-1) % inopgrp;
        return &get_inode_table(g)[j];

    }



    void write_file(FILE *file,const std::string& file_name){
        unsigned char buffer[1024];
        struct stat sb{};
        size_t bytesRead = 0;
        int fd = fileno(file);
        std::vector<uint32_t > blknums;
        // Write to datablocks
        while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0){
            blknums.emplace_back(write_data_blk(bytesRead,buffer));
        }
        if(fstat(fd,&sb) == -1){
            std::perror("fstat error");

        }
        auto idx_free_inode = get_free_inode();
        auto*inode = get_inode(idx_free_inode);
        // Write to inode
        // File related stuff
        inode->i_mode = sb.st_mode;
        inode->i_uid = sb.st_uid & 0xFFFF;
        inode->i_gid = sb.st_gid & 0xFFFF;
        inode->i_size = sb.st_size;
        inode->i_blocks = sb.st_blocks;
        inode->i_atime = sb.st_atim.tv_sec;
        inode->i_mtime = sb.st_mtim.tv_sec;
        inode->i_ctime = sb.st_ctim.tv_sec;
        // Misc. for inode
        inode->i_links_count = 1;
        inode->i_flags = 1;
        unsigned int blk_size = blknums.size();
        unsigned int cnt = std::min(blk_size,12u);
        for(int i =0; i < cnt;i++){
            inode->i_block[i] = blknums[i];
        }
        unsigned int pointers_per_block = blksz / 4;
        // Indirect block
        if(blk_size > 12 ){
            blk_size -= 12;
            cnt = std::min(blk_size,pointers_per_block);
            auto ind_block = get_free_block();
            auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
            inode->i_block[12] = ind_block;
            for(int i =0; i < cnt;i++){
                block[i] = blknums[12+i];
            }
        }
        // Double Indirect Block
        if(blk_size > pointers_per_block){
            blk_size -= pointers_per_block;
            cnt = std::min(blk_size,pointers_per_block*pointers_per_block);
            auto d_ind_block = get_free_block();
            auto * dblock = reinterpret_cast<uint32_t *>(get_block(d_ind_block));
            inode->i_block[13] = d_ind_block;
            for(int i = 0; i < pointers_per_block;i++){
                auto ind_block = get_free_block();
                auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
                dblock[i] = ind_block;
                for(int j=0; j < pointers_per_block;j++ ){
                    block[j] = blknums[12 + pointers_per_block + j];
                    cnt--;
                    if(!cnt){
                        goto triple;
                    }
                }
            }



        }
        triple:
        if(blk_size >  pointers_per_block*pointers_per_block){
            // Triple Indirect BLock
            blk_size -= pointers_per_block*pointers_per_block;
            cnt = std::min(blk_size,pointers_per_block*pointers_per_block*pointers_per_block);
            auto t_ind_block = get_free_block();
            auto * tblock = reinterpret_cast<uint32_t *>(get_block(t_ind_block));
            inode->i_block[14] = t_ind_block;
            for(int i=0; i< pointers_per_block; i++){
                auto d_ind_block = get_free_block();
                auto * dblock = reinterpret_cast<uint32_t *>(get_block(d_ind_block));
                tblock[i] = d_ind_block;
                for(int j=0; j < pointers_per_block; j++){
                    auto ind_block = get_free_block();
                    auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
                    dblock[j] = ind_block;
                    for(int k = 0; k < pointers_per_block; k++){
                        block[k] = blknums[12 + pointers_per_block*pointers_per_block + k];
                        cnt--;
                        if(!cnt){
                            goto directory;
                        }
                    }
                }
            }

        }

        // Write to directory entries
        directory:
        auto * root = get_inode(2);
        for(int i= 0; i < 12; i++){
            unsigned int blk_num = root->i_block[i];
            if(blk_num ==0){
                blk_num = get_free_block();
                root->i_blocks +=  (blksz / 512);
                root->i_size += blksz;
                root->i_block[i] = blk_num;
            }
            unsigned int pad = (4-  (file_name.length() % 4)) % 4;
            unsigned int required_bytes = 4 + file_name.length() + pad;
            auto  dir =  get_free_dir_entry(blk_num,required_bytes);
            auto* dir_entry = reinterpret_cast<ext2_dir_entry*>(std::get<0>(dir));
            if(!dir_entry){
                continue;
            }
            uint32_t rec_len = std::get<1>(dir);
            dir_entry->inode = idx_free_inode;
            dir_entry->rec_len = rec_len;
            dir_entry->name_len = file_name.length();

            //Change this later
            dir_entry->file_type = EXT2_FT_REG_FILE;
            std::memcpy(dir_entry->name,file_name.c_str(), dir_entry->name_len);
            return;
        }




    }

    std::tuple<char* ,uint32_t >get_free_dir_entry(uint32_t block_no,uint32_t required_bytes){
        auto* dir_offset = get_block(block_no);
        auto  * dir = reinterpret_cast<ext2_dir_entry*>(dir_offset);
        unsigned int pad =  (4-  (dir->name_len % 4)) % 4;
        unsigned int cur_req = 8 + dir->name_len + pad;
        unsigned int read_bytes = 0;
        while(dir ->rec_len < required_bytes + cur_req && dir->inode){
            if(read_bytes >= blksz){
                return std::make_tuple(nullptr,0);
            }
            dir_offset += dir->rec_len;
            dir = reinterpret_cast<ext2_dir_entry*>(dir_offset);
            pad =  (4-  (dir->name_len % 4)) % 4;
            cur_req = 8 + dir->name_len + pad;
            read_bytes += dir->rec_len;

        }
        if(!dir->inode){
            return std::make_tuple(dir_offset,blksz);
        }else{
            auto tup = std::make_tuple(dir_offset + cur_req,dir->rec_len  - cur_req);
            dir->rec_len = cur_req;
            return tup;
        }


    }
    uint32_t get_free_block(){
        uint32_t idx = 1;
        for(int i=0; i < ngroups; ++i){
            auto bat = get_bat(i);
            for(int j=0; j < blkpgrp;j++){
                if(!bat.is_set(j)){
                    bat.set(j);
                    return idx;
                }
                idx++;

            }
        }
        return 0;
    }
    uint32_t get_free_inode(){
        uint32_t idx = 1;
        for(int i=0; i < ngroups; ++i){
            auto iat = get_iat(i);
            for(int j=0; j < inopgrp;j++){
                if(!iat.is_set(j)){
                    iat.set(j);
                    return idx;
                }
                idx++;

            }
        }
        return 0;

    }
    uint32_t write_data_blk(size_t bytes,unsigned char* buffer){
        auto idx_free_blk = get_free_block();
        std::memcpy(get_block(idx_free_blk),buffer,bytes);
        return idx_free_blk;


    }


};

bool isDir(const std::string& dir){
    struct stat fileInfo;
    stat(dir.c_str(), &fileInfo);
    if (S_ISDIR(fileInfo.st_mode)){
        return true;
    }else{
        return false;
    }
}

void getdir(std::string dir, std::vector<std::tuple<std::string,std::string>> &files, bool recursive,const std::string& parent){
    DIR *dp; //create the directory object
    struct dirent *entry; //create the entry structure
    dp=opendir(dir.c_str()); //open directory by converting the string to const char*
    if(dir.at(dir.length()-1)!='/'){
        dir=dir+"/";
    }
    if(dp!=nullptr){ //if the directory isn't empty
        entry=readdir(dp);
        while(entry){ //while there is something in the directory
            if(strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0){ //and if the entry isn't "." or ".."
                if (isDir(dir + entry->d_name)) {
                    if (recursive) {//check if the new path is a directory, and if it is (and recursion is specified as true), recurse.
                        files.emplace_back(std::make_tuple(entry->d_name,dir)); //add entry to the list of files
                        getdir(dir + entry->d_name, files, true,dir); //recurse
                    } else {
                        files.emplace_back(std::make_tuple(entry->d_name,parent));//add the entry to the list of files
                    }
                } else {
                    files.emplace_back(std::make_tuple(entry->d_name,dir));
                }
            }
            entry=readdir(dp);
        }
        (void) closedir(dp); //close directory
    }
    else{
        perror ("Couldn't open the directory.");
    }
}

int main(int argc,char *argv[]) {
    /*
    auto * img = load_image(argv[1]);
    auto fs = Filesystem(img);
    FILE *file = nullptr;
    file = fopen("loremloremloremloremloremloremloremloremloremloremloremloremloremloremloremlorem12.txt","r");
    fs.write_file(file,"loremloremloremloremloremloremloremloremloremloremloremloremloremloremloremlorem12.txt");
    fclose(file);
     */
    std::vector<std::tuple<std::string,std::string>> files{};
    getdir("./test",files,true,"none");
    int a = 4;

    return 0;
}