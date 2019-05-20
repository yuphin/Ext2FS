

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
#include <queue>
#include <unordered_map>
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
        sup = reinterpret_cast<ext2_super_block*>(get_super_block());
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
        auto *dir = reinterpret_cast<ext2_dir_entry*>(get_block(569));
        auto *inode = get_inode(366);
        inode_bitmap_size =  inopgrp / 8;
        block_bitmap_size = blkpgrp / 8;
    }

    char* get_block(unsigned int block_id){
        return  img + block_id * this->blksz;
    }

    char* get_super_block(){
        return img + BASE_OFFSET;
    }

    uint32_t find_inode(uint32_t inode,const std::string& dir_name){

        auto *dir_inode = get_inode(inode);
        for(int i=0; i < 12 ; i++){
            auto *dir_offset = get_block(dir_inode->i_block[i]);
            auto  * dir = reinterpret_cast<ext2_dir_entry*>(dir_offset);
            unsigned int read_bytes = dir->rec_len;
            while(read_bytes < blksz){
                auto str = std::string(dir->name,dir->name_len);
                if(str == dir_name){
                    return dir->inode;
                }
                read_bytes += dir->rec_len;
                dir_offset += dir->rec_len;
                dir = reinterpret_cast<ext2_dir_entry*>(dir_offset);
            }

        }
        return 0;



    }

    uint32_t get_dir_inode(std::basic_string<char> path){
        if(path == "/"){
            return 2;
        }
        if(path[0] == '/'){
            path.erase(path.begin());
        }
        if(path[path.length()-1] != '/'){
            path += "/";
        }
        std::string delimiter = "/";
        size_t pos = 0;
        std::string token;
        uint32_t inode = 2;
        while ((pos = path.find(delimiter)) != std::string::npos) {
            token = path.substr(0, pos);
            inode = find_inode(inode,token);
            path.erase(0, pos + delimiter.length());
        }

        return inode;
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



    uint32_t write_file(const std::string& file_name,struct stat& sb,FILE* file = nullptr,uint32_t parent_inode=2){
        unsigned char buffer[blksz];
        size_t bytesRead = 0;
        std::vector<uint32_t > blknums;
        auto idx_free_inode = get_free_inode();
        auto*inode = get_inode(idx_free_inode);


        // Write to inode //
        // File related stuff
        inode->i_mode = sb.st_mode;
        inode->i_uid = sb.st_uid & 0xFFFF;
        inode->i_gid = sb.st_gid & 0xFFFF;

        inode->i_atime = sb.st_atim.tv_sec;
        inode->i_mtime = sb.st_mtim.tv_sec;
        inode->i_ctime = sb.st_ctim.tv_sec;
        inode->i_flags = 1;


        // Misc. for inode
        if(S_ISREG(sb.st_mode)){
            inode->i_links_count = 1;
            inode->i_size =  sb.st_size;
            inode->i_blocks = 0;
            std::vector<unsigned int> blocks;
            // Write to datablocks
            while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0){
                blknums.emplace_back(write_data_blk(bytesRead,buffer));
            }
            unsigned int blk_size = blknums.size();
            unsigned int cnt = std::min(blk_size,12u);
            for(int i =0; i < cnt;i++){
                inode->i_block[i] = blknums[i];
                inode->i_blocks += (blksz / 512);
                blocks.emplace_back(blknums[i]);
            }
            unsigned int pointers_per_block = blksz / 4;
            // Indirect block
            if(blk_size > 12 ){
                inode->i_blocks += (blksz / 512);
                blk_size -= 12;
                cnt = std::min(blk_size,pointers_per_block);
                auto ind_block = get_free_block();
                blocks.emplace_back(ind_block);
                auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
                inode->i_block[12] = ind_block;
                for(int i =0; i < cnt;i++){
                    inode->i_blocks += (blksz / 512);
                    block[i] = blknums[12+i];
                    blocks.emplace_back(blknums[12+i]);
                }
            }
            // Double Indirect Block
            if(blk_size > pointers_per_block){
                blk_size -= pointers_per_block;
                inode->i_blocks += (blksz / 512);
                cnt = std::min(blk_size,pointers_per_block*pointers_per_block);
                auto d_ind_block = get_free_block();
                blocks.emplace_back(d_ind_block);
                auto * dblock = reinterpret_cast<uint32_t *>(get_block(d_ind_block));
                inode->i_block[13] = d_ind_block;
                for(int i = 0; i < pointers_per_block;i++){
                    auto ind_block = get_free_block();
                    auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
                    blocks.emplace_back(ind_block);
                    inode->i_blocks += (blksz / 512);
                    dblock[i] = ind_block;
                    for(int j=0; j < pointers_per_block;j++ ){
                        block[j] = blknums[12 + pointers_per_block + j];
                        blocks.emplace_back(blknums[12 + pointers_per_block + j]);
                        inode->i_blocks += (blksz / 512);
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
                inode->i_blocks += (blksz / 512);
                cnt = std::min(blk_size,pointers_per_block*pointers_per_block*pointers_per_block);
                auto t_ind_block = get_free_block();
                blocks.emplace_back(t_ind_block);
                auto * tblock = reinterpret_cast<uint32_t *>(get_block(t_ind_block));
                inode->i_block[14] = t_ind_block;
                for(int i=0; i< pointers_per_block; i++){
                    auto d_ind_block = get_free_block();
                    auto * dblock = reinterpret_cast<uint32_t *>(get_block(d_ind_block));
                    blocks.emplace_back(d_ind_block);
                    inode->i_blocks += (blksz / 512);
                    tblock[i] = d_ind_block;
                    for(int j=0; j < pointers_per_block; j++){
                        auto ind_block = get_free_block();
                        auto * block = reinterpret_cast<uint32_t *>(get_block(ind_block));
                        blocks.emplace_back(ind_block);
                        inode->i_blocks += (blksz / 512);
                        dblock[j] = ind_block;
                        for(int k = 0; k < pointers_per_block; k++){
                            block[k] = blknums[12 + pointers_per_block*pointers_per_block + k];
                            blocks.emplace_back(blknums[12 + pointers_per_block*pointers_per_block + k]);
                            inode->i_blocks += (blksz / 512);
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
            auto * root = get_inode(parent_inode);
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
                dir_entry->file_type = EXT2_FT_REG_FILE;
                std::memcpy(dir_entry->name,file_name.c_str(), dir_entry->name_len);
                fclose(file);
                std::cout << idx_free_inode << " ";
                for(int idx=0; idx< blocks.size();idx++){
                    if(idx != blocks.size()-1){
                        std::cout << blocks[idx] << " ";
                    }else{
                        std::cout << blocks[idx] << std::endl;
                    }

                }
                return 0;
            }

        }else if(S_ISDIR(sb.st_mode)){
            auto * parent = get_inode(parent_inode);

            inode->i_links_count = 2;
            inode->i_size =0;
            inode->i_blocks = 0;
            parent->i_links_count ++;
            for(int i= 0; i < 12; i++){
                unsigned int blk_num_parent= parent->i_block[i];
                if(blk_num_parent == 0){
                    blk_num_parent = get_free_block();
                    parent->i_blocks +=  (blksz / 512);
                    parent->i_size += blksz;
                    parent->i_block[i] = blk_num_parent;
                }
                unsigned int pad = (4-  (file_name.length() % 4)) % 4;
                unsigned int required_bytes = 4 + file_name.length() + pad;
                auto dir_parent = get_free_dir_entry(blk_num_parent,required_bytes);
                auto* dir_entry_parent =reinterpret_cast<ext2_dir_entry*>(std::get<0>(dir_parent));
                if(!dir_entry_parent){
                    continue;
                }

                uint32_t rec_len_parent = std::get<1>(dir_parent);
                dir_entry_parent->inode = idx_free_inode;
                dir_entry_parent->rec_len = rec_len_parent;
                dir_entry_parent->name_len = file_name.length();
                dir_entry_parent->file_type = EXT2_FT_DIR;
                std::memcpy(dir_entry_parent->name,file_name.c_str(), dir_entry_parent->name_len);
                break;
            }

            for(int i= 0; i < 12; i++){
                unsigned int blk_num = inode->i_block[i];
                if(blk_num ==0){
                    blk_num = get_free_block();
                    inode->i_blocks +=  (blksz / 512);
                    inode->i_size += blksz;
                    inode->i_block[i] = blk_num;
                }

                unsigned int required_bytes = 12;
                auto  dir1 =  get_free_dir_entry(blk_num,required_bytes);
                auto* dir_entry1 = reinterpret_cast<ext2_dir_entry*>(std::get<0>(dir1));
                if(!dir_entry1){
                    continue;
                }
                uint32_t rec_len1 = std::get<1>(dir1);
                dir_entry1->inode = idx_free_inode;
                dir_entry1->rec_len = rec_len1;
                dir_entry1->name_len = 1;
                dir_entry1->file_type = EXT2_FT_DIR;
                dir_entry1->name[0] = '.';


                auto  dir2 =  get_free_dir_entry(blk_num,required_bytes);
                auto* dir_entry2 = reinterpret_cast<ext2_dir_entry*>(std::get<0>(dir2));
                uint32_t rec_len2 = std::get<1>(dir2);
                dir_entry2->inode = parent_inode;
                dir_entry2->rec_len = rec_len2;
                dir_entry2->name_len = 2;
                dir_entry2->file_type = EXT2_FT_DIR;
                dir_entry2->name[0] = '.';
                dir_entry2->name[1] = '.';


                gdt[idx_free_inode / inopgrp].bg_used_dirs_count++;
                return idx_free_inode;
            }
        }






    }

    std::tuple<char* ,uint32_t >get_free_dir_entry(uint32_t block_no,uint32_t required_bytes){
        auto* dir_offset = get_block(block_no);
        auto  * dir = reinterpret_cast<ext2_dir_entry*>(dir_offset);
        unsigned int pad =  (4-  (dir->name_len % 4)) % 4;
        unsigned int cur_req = 8 + dir->name_len + pad;
        unsigned int read_bytes = dir->rec_len;
        while(dir ->rec_len < required_bytes + cur_req && dir->inode){
            if(dir->rec_len == 328){
                int b = 4;
            }
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
            if(dir->rec_len  - cur_req == 232){
                int a = 4;
            }
            dir->rec_len = cur_req;
            return tup;
        }


    }
    uint32_t get_free_block(){
        uint32_t idx = sup->s_first_data_block;
        for(int i=0; i < ngroups; ++i){
            auto bat = get_bat(i);
            for(int j=0; j < blkpgrp;j++){
                if(!bat.is_set(j)){
                    bat.set(j);
                    gdt[i].bg_free_blocks_count--;
                    sup->s_free_blocks_count --;
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
                    gdt[i].bg_free_inodes_count--;
                    sup->s_free_inodes_count --;
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

bool isDir(const std::string& dir,struct stat& fileInfo){

    stat(dir.c_str(), &fileInfo);
    if (S_ISDIR(fileInfo.st_mode)){
        return true;
    }else{
        return false;
    }
}


std::unordered_map<uint32_t,uint32_t > umap;
std::unordered_map<uint32_t,uint32_t > pmap;
std::unordered_map<std::string,std::string> fmap;
int incr = -1;
void getdir(std::string dir, uint32_t parent,Filesystem& fs,
            std::priority_queue<
                    std::tuple<std::string,unsigned int>,
                    std::vector<std::tuple<std::string,unsigned int>>,
                    std::greater<>>& queue){
    DIR *dp; //create the directory object
    struct dirent *entry; //create the entry structure
    dp=opendir(dir.c_str()); //open directory by converting the string to const char*
    if(dir.at(dir.length()-1)!='/'){
        dir=dir+"/";
    }
    if(dp!=nullptr){
        entry=readdir(dp);
        while(entry){ //while there is something in the directory
            if(strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0){ //and if the entry isn't "." or ".."
                struct stat fileInfo{};
                if (isDir(dir + entry->d_name,fileInfo)) {

                    //uint32_t inode = fs.write_file(entry->d_name,fileInfo,nullptr,parent);
                    incr--;
                    if(incr == parent)
                        incr--;
                    queue.push(std::make_tuple(dir + entry->d_name,incr));
                    umap.insert(std::make_pair(incr,parent));
                    pmap.insert(std::make_pair(incr,incr));
                    fmap.insert(std::make_pair(dir + entry->d_name,entry->d_name));
                    getdir(dir + entry->d_name,incr,fs,queue); //recurse


                } else {
                    queue.push(std::make_tuple(dir + entry->d_name,parent));
                    fmap.insert(std::make_pair(dir + entry->d_name,entry->d_name));
                    //FILE *file = fopen((dir + entry->d_name).c_str(),"r");
                    //fs.write_file(entry->d_name,fileInfo,file,parent);
                    //std::cout << "Read " << entry->d_name << std::endl;
                }
            }
            entry=readdir(dp);
        }
        (void) closedir(dp); //close directory
    }
    else{
        //perror ("Couldn't open the directory.");
        dir.pop_back();
        queue.push(std::make_tuple(dir,parent));
        fmap.insert(std::make_pair(dir,dir));

    }
}
template<typename T> void process_queue(T &q,Filesystem&fs) {
    while(!q.empty()) {
        auto a = q.top();
        struct stat fileInfo{};
        FILE *file = fopen((std::get<0>(a)).c_str(),"r");
        if(isDir(std::get<0>(a),fileInfo)){

            auto inode = fs.write_file(fmap[std::get<0>(a)],fileInfo,nullptr,umap[std::get<1>(a)]);
            //std::cout << std::get<0>(a) << " " << inode << "\n";
            pmap[std::get<1>(a)] = inode;
            for(auto& m : umap){
                if(m.second == std::get<1>(a)){
                    m.second = inode;
                }
            }
        }else{
            if(umap.find(pmap[std::get<1>(a)]) != umap.end()){
                fs.write_file(fmap[std::get<0>(a)],fileInfo,file,pmap[umap[std::get<1>(a)]]);
                //std::cout << std::get<0>(a) << " " << umap[pmap[std::get<1>(a)]] << "\n";
            }else{
                fs.write_file(fmap[std::get<0>(a)],fileInfo,file,pmap[std::get<1>(a)]);
                //std::cout << std::get<0>(a) << " " << pmap[std::get<1>(a)] << "\n";
            }

        }
        //fs.write_file(std::get<0>(a),fileInfo,file,std::get<1>(a));

        q.pop();
    }
    //std::cout << '\n';
}
int main(int argc,char *argv[]) {
    uint32_t inode;
    auto * img = load_image(argv[1]);
    auto fs = Filesystem(img);
    try{
        inode = std::stoi(argv[3]);
    }catch(const std::invalid_argument& ia){
        inode = fs.get_dir_inode(argv[3]);
    }
    pmap.insert(std::make_pair(inode,inode));
    std::priority_queue <std::tuple<std::string,unsigned int>,std::vector<std::tuple<std::string,unsigned int>>,std::greater<>> q;
    getdir(argv[2],inode,fs,q);
    process_queue(q,fs);
    return 0;
}