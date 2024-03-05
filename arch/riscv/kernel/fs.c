#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "slub.h"
#include "task_manager.h"
#include "virtio.h"
#include "vm.h"
#include "mm.h"

// --------------------------------------------------
// ----------- read and write interface -------------

void disk_op(int blockno, uint8_t *data, bool write) {
    struct buf b;
    b.disk = 0;
    b.blockno = blockno;
    b.data = (uint8_t *)PHYSICAL_ADDR(data);
    virtio_disk_rw((struct buf *)(PHYSICAL_ADDR(&b)), write);
}

#define disk_read(blockno, data) disk_op((blockno), (data), 0)
#define disk_write(blockno, data) disk_op((blockno), (data), 1)

// -------------------------------------------------
// ------------------ your code --------------------

//int Mem_used = 0;

int streql(char* a, char* b){
    for(int i = 0; a[i] != '\0' && b[i] != '\0'; i++){
        if(a[i] != b[i])return 0;
    }
    return 1;
}

int strsize(char* a){
    int len=0;
    while(a[len]!='\0')len++;
    return len;
}

struct sfs_fs* fs = NULL;

Mblock inode_to_mem(INODE din, int no){
    Mblock mem = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    mem->block.din = din;
    mem->kind = DIN;
    mem->blockno = no;
    mem->dirty = 0;
    mem->reclaim_count = din->links;
    return mem;
}

Mblock entry_to_mem(ENTRY den, int no){
    Mblock mem = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    mem->block.den = den;
    mem->kind = DEN;
    mem->blockno = no;
    mem->dirty = 0;
    mem->reclaim_count = 1;
    return mem;
}

Mblock data_to_mem(char* data, int no){
    Mblock mem = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    mem->block.block = (char*)kmalloc(4097);
//    Mem_used += 4097;
    memcpy(mem->block.block, data, strsize(data));
    mem->kind = BLOCK;
    mem->blockno = no;
    mem->dirty = 0;
    mem->reclaim_count = 1;
    return mem;
}

int write_back(Mblock mem){
    if(!(mem->dirty)){
        if((!fs->hash[mem->blockno%256])&&(fs->hash[mem->blockno%256]==mem->blockno)) {
            kfree(fs->hash[mem->blockno % 256]);
//            Mem_used -= sizeof(sfs_memory_block);
            fs->hash[mem->blockno % 256] = NULL;
        }
        return 1;
    }

    switch(mem->kind){
        case 0:
            disk_write(mem->blockno, mem->block.block);
            kfree(mem->block.block);
//            Mem_used -= sizeof(4097);
            break;
        case 1:
            disk_write(mem->blockno, (char*)mem->block.din);
            kfree(mem->block.din);
//            Mem_used -= sizeof(sfs_inode);
            break;
        case 2:
            disk_write(mem->blockno, (char*)mem->block.den);
            kfree(mem->block.den);
//            Mem_used -= sizeof(sfs_entry);
            break;
    }
    kfree(fs->hash[mem->blockno%256]);
//    Mem_used -= sizeof(sfs_memory_block);
    fs->hash[mem->blockno%256] = NULL;
    return 1;
}

void add_cache(Mblock mem){
    int no = mem->blockno;
    if(!fs->hash[no%256]){
        fs->hash[no%256] = mem;
    }
    else if(fs->hash[no%256]->blockno != no){
        write_back(fs->hash[no%256]);
        fs->hash[no%256] = mem;
    }
}

uint32_t find_freeblock(){
    uint8_t And = 0b11111111, one = 0b00000001;
    uint32_t i;
    for(i = 0; i < 4096; i++)
        if((fs->freemap[i] & And) != And)break;
    uint8_t space = (fs->freemap[i] & And) + 1;
    uint32_t area = i * 8;
    while(!(space & one)){
        one <<= 1;
        area++;
    }
    fs->freemap[i] |= one;
    return area;
}

Mblock find_block(uint32_t num, uint16_t type)
{
    if(!fs->hash[num%256] || fs->hash[num%256]->blockno != num){
        switch(type){
            case DIN: {
                INODE block = (INODE) kmalloc(sizeof(struct sfs_inode));
//                Mem_used += sizeof(sfs_inode);
                disk_read(num, (uint8_t*) block);
                Mblock mem = inode_to_mem(block, num);
                add_cache(mem);
                break;
            }
            case DEN: {
                ENTRY block = (ENTRY) kmalloc(sizeof(struct sfs_entry));
//                Mem_used += sizeof(sfs_entry);
                disk_read(num, (uint8_t*) block);
                Mblock mem = entry_to_mem(block, num);
                add_cache(mem);
                break;
            }
            case BLOCK: {
                char* data = (char*)kmalloc(4097);
//                Mem_used += 4097;
                disk_read(num, (uint8_t*)data);
                data[4096] = '\0';
                Mblock mem = data_to_mem(data, num);
                add_cache(mem);
                break;
            }
        }
    }
    return fs->hash[num%256];
}

Mblock find_file(char* name, Mblock dir){
    if(!dir->block.din->type)return NULL;
    for (uint32_t i = 0; i < dir->block.din->blocks; i++) {
        Mblock mem = find_block(dir->block.din->direct[i], DEN);
        if(streql(mem->block.den->filename, name)) {
            Mblock file = find_block(mem->block.den->ino, DIN);
            return file;
        }
    }
    if(!dir->block.din->indirect){
        return NULL;
    }
    else{
        Mblock mem = find_block(dir->block.din->indirect, DIN);
        return find_file(name, mem);
    }
}

Mblock create_entry(char* name, uint32_t no){
    ENTRY den = (ENTRY)kmalloc(sizeof(struct sfs_entry));
//    Mem_used += sizeof(sfs_entry);
    Mblock mem_entry = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    mem_entry->kind = DEN;
    mem_entry->blockno = find_freeblock();
    mem_entry->dirty = 1;
    mem_entry->reclaim_count = 1;
    mem_entry->block.den = den;
    den->ino = no;
    memcpy(den->filename, name, strsize(name));
    add_cache(mem_entry);
    return mem_entry;
}

Mblock create_inode(uint16_t type){
    INODE din = (INODE)kmalloc(sizeof(struct sfs_inode));
//    Mem_used += sizeof(sfs_inode);
    Mblock mem = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    din->size = 0;
    din->type = type;
    din->links = 1;
    din->blocks = 0;
    mem->kind = DIN;
    mem->blockno = find_freeblock();
    mem->dirty = 1;
    mem->reclaim_count = 1;
    mem->block.din = din;
    add_cache(mem);
    return mem;
}

Mblock create_data(){
    Mblock mem = (Mblock)kmalloc(sizeof(struct sfs_memory_block));
//    Mem_used += sizeof(sfs_memory_block);
    mem->kind = BLOCK;
    mem->block.block = (char*)kmalloc(4097);
//    Mem_used += sizeof(4097);
    mem->block.block[4096]='\0';
    mem->blockno = find_freeblock();
    mem->dirty = 1;
    mem->reclaim_count = 1;
    add_cache(mem);
    return mem;
}

Mblock create_dir(char* name, Mblock dir){
    Mblock mem_inode = create_inode(1);
    Mblock mem_entry0 = create_entry(name, mem_inode->blockno);
    Mblock mem_entry1 = create_entry(".", mem_inode->blockno);
    Mblock mem_entry2 = create_entry("..", dir->blockno);
    mem_inode->block.din->direct[0] = mem_entry1->blockno;
    mem_inode->block.din->direct[1] = mem_entry2->blockno;
    mem_inode->block.din->blocks = 2;
    Mblock mem = dir;
    while(mem->block.din->indirect){
        mem = find_block(mem->block.din->indirect, DIN);
    }
    if(mem->block.din->blocks < SFS_NDIRECT){
        mem->block.din->direct[mem->block.din->blocks++] = mem_entry0->blockno;
    }
    else{
        Mblock new_inode = create_inode(1);
        mem->block.din->indirect = new_inode->blockno;
        new_inode->block.din->direct[0] = mem_entry0->blockno;
        new_inode->block.din->blocks++;
    }
    return mem_inode;
}

Mblock create_file(char* name, Mblock dir){
    Mblock mem_inode = create_inode(0);
    Mblock mem_entry = create_entry(name, mem_inode->blockno);
    Mblock mem_data = create_data();
    mem_inode->block.din->blocks = 1;
    mem_inode->block.din->direct[0] = mem_data->blockno;
    Mblock mem = dir;
    while(mem->block.din->indirect){
        mem = find_block(mem->block.din->indirect, DIN);
    }
    if(mem->block.din->blocks < SFS_NDIRECT){
        mem->block.din->direct[mem->block.din->blocks++] = mem_entry->blockno;
    }
    else{
        Mblock new_inode = create_inode(1);
        mem->block.din->indirect = new_inode->blockno;
        new_inode->block.din->direct[0] = mem_entry->blockno;
        new_inode->block.din->blocks++;
    }

    return mem_inode;
}

int sfs_init(){//ok
    fs = (struct sfs_fs*)kmalloc(sizeof(struct sfs_fs));
//    Mem_used += sizeof(sfs_fs);
    for(int i=0;i<256;i++)fs->hash[i] = NULL;
    fs->size = 0;
    fs->super_dirty = 0;
    INODE root = (INODE)kmalloc(sizeof(struct sfs_inode));
//    Mem_used += sizeof(sfs_inode);
    disk_read(0, (uint8_t*)&(fs->super));
    disk_read(1, (uint8_t*)root);
    Mblock mem = inode_to_mem(root, 1);
    add_cache(mem);
    disk_read(2, (uint8_t*)fs->freemap);
//    printf("Mem:%d\n",Mem_used);
    return 0;
}

int sfs_open(const char *path, uint32_t flags){//ok
    if(!fs)sfs_init();
    int i;
    for(i=0;i<16;i++){
        if(!current->fs.fds[i])break;
    }
    if(i==16){
        printf("The process has opened so many files!\n");
        return -1;
    }
    int size = strsize(path), lst=1, num=0;
    if(path[0]!='/'){
        printf("Invalid path\n");
        return -1;
    }
    Mblock cur = find_block(1, DIN);
    for(int i = 1; i < size; i++){
        if(path[i]=='/'){
            int len = i - lst;
            char* name = (char*)kmalloc(len+1);
//            Mem_used += len+1;
            for(int j=0;j<len;j++)name[j]=path[lst+j];
            name[len]='\0';
            Mblock mem = find_file(name, cur);
            if(!mem&&!(flags&SFS_FLAG_WRITE))return -1;
            else if(!mem&&(flags&SFS_FLAG_WRITE)){
                cur = create_dir(name, cur);
            }
            else cur = mem;
            lst = i+1;
        }
    }
    int len = size - lst;
    char* name = (char*)kmalloc(len+1);
//    Mem_used += len+1;
    for(int i=0;i<len;i++)name[i]=path[lst+i];
    name[len]='\0';
    Mblock mem = find_file(name, cur);
    if(!mem&&!(flags&SFS_FLAG_WRITE))return -1;
    else if(!mem&&(flags&SFS_FLAG_WRITE)){
        mem = create_file(name, cur);
    }
    current->fs.fds[i] = (struct file*)kmalloc(sizeof(struct file));
//    Mem_used += sizeof(struct file);
    current->fs.fds[i]->inode_no = mem->blockno;
    current->fs.fds[i]->off = 0;
    current->fs.fds[i]->flags = flags;
    current->fs.fds[i]->inode = (INODE)kmalloc(sizeof(struct sfs_inode));
//    Mem_used += sizeof(struct sfs_inode);
    memcpy(current->fs.fds[i]->inode, mem->block.din, sizeof(struct sfs_inode));
    return 0;
}

int sfs_close(int fd){
    if(!fs)sfs_init();
    struct file* f = current->fs.fds[fd];
    if(!f)return 1;
    if(f->inode->type){
        for(int i=2;i<f->inode->blocks;i++){
            if(!fs->hash[f->inode->direct[i]%256])continue;
            if(fs->hash[f->inode->direct[i]%256]->blockno == f->inode->direct[i])
                write_back(fs->hash[f->inode->direct[i]%256]);
        }
    }
    else{
        for(int i=0;i<f->inode->blocks;i++){
            if(!fs->hash[f->inode->direct[i]%256])continue;
            if(fs->hash[f->inode->direct[i]%256]->blockno == f->inode->direct[i])
                write_back(fs->hash[f->inode->direct[i]%256]);
        }
    }
    if(fs->hash[f->inode_no%256]->blockno == f->inode_no){
        write_back(fs->hash[f->inode_no%256]);
    }
    kfree(f->inode);
//    Mem_used -= sizeof(sfs_inode);
    kfree(f);
//    Mem_used -= sizeof(sfs_memory_block);
    current->fs.fds[fd] = NULL;
//    printf("Mem:%d\n",Mem_used);
    return 0;
}

int sfs_seek(int fd, int32_t off, int fromwhere){
    if(!fs)sfs_init();
    uint32_t size = current->fs.fds[fd]->inode->size;
    switch(fromwhere){
        case SEEK_SET: {
            if (off > size) {
                printf("The offset beyond the file!\n");
                return -1;
            }
            current->fs.fds[fd]->off = off;
            break;
        }
        case SEEK_CUR: {
            if (current->fs.fds[fd]->off + off > size) {
                printf("The offset beyond the file!\n");
                return -1;
            }
            current->fs.fds[fd]->off += off;
            break;
        }
        case SEEK_END:{
            if(off > size){
                printf("The offset beyond the file!\n");
                return -1;
            }
            current->fs.fds[fd]->off = size - off;
            break;
        }
    }
    return 0;
}

int sfs_read(int fd, char *buf, uint32_t len){
    if(!fs)sfs_init();
    struct file* f = current->fs.fds[fd];
    if(!f){
        printf("There isn't a file!\n");
        return -1;
    }
    if(f->inode->type){
        printf("The file is a directory\n");
        return -1;
    }
    if(!(f->flags&SFS_FLAG_READ)){
        printf("The file can't be read\n");
        return -1;
    }
    if(f->off == f->inode->size){
        return 0;
    }
    if(f->off + len > f->inode->size)
        len = f->inode->size - f->off;
    int no = f->off / 4096;
    Mblock cur = find_block(f->inode_no, DIN);
    while(no >= SFS_NDIRECT){
        no -= SFS_NDIRECT;
        cur = find_block(cur->block.din->indirect, DIN);
    }
    uint32_t finish = 0, offset = f->off % 4096;
    while(finish < len){
        Mblock mem = find_block(cur->block.din->direct[no++], BLOCK);
        for(int i = 0; i < min(4096-offset, len - finish); i++){
            buf[finish+i] = mem->block.block[offset+i];
        }
        finish += min(4096 - offset, len - finish);
        offset = 0;
        if(no == SFS_NDIRECT){
            no -= SFS_NDIRECT;
            cur = find_block(cur->block.din->indirect, DIN);
        }
    }
    f->off += len;
    return len;
}

int sfs_write(int fd, char *buf, uint32_t len){
    if(!fs)sfs_init();
    struct file* f = current->fs.fds[fd];
    if(!f){
        printf("There isn't a file!\n");
        return 1;
    }
    if(f->inode->type){
        printf("The file is a directory\n");
        return 1;
    }
    if(!((f->flags&SFS_FLAG_READ)&&(f->flags&SFS_FLAG_WRITE))){
        printf("The file can't be write\n");
        return 1;
    }
    int no = f->off / 4096;
    Mblock cur = find_block(f->inode_no, DIN);
    while(no >= SFS_NDIRECT){
        no -= cur->block.din->blocks;
        cur = find_block(cur->block.din->indirect, DIN);
    }
    uint32_t finish = 0, offset = f->off % 4096;
    while(finish < len){
        Mblock mem = NULL;
        if(cur->block.din->direct[no])mem = find_block(cur->block.din->direct[no], BLOCK);
        else{
            mem = create_data();
            cur->block.din->direct[no] = mem->blockno;
            cur->block.din->blocks++;
        }
        no++;
        for(int i = 0; i < min(4096-offset, len - finish); i++){
            mem->block.block[offset+i] = buf[finish+i];
        }
        finish += min(4096 - offset, len - finish);
        offset = 0;
        mem->dirty = 1;

        if(no == SFS_NDIRECT){
            no -= SFS_NDIRECT;
            if(cur->block.din->indirect)cur = find_block(cur->block.din->indirect, DIN);
            else{
                mem = create_inode(0);
                cur->block.din->indirect = mem->blockno;
                cur = mem;
            }
        }
    }
    if(f->off + len > f->inode->size){
        cur = find_block(f->inode_no, DIN);
        cur->block.din->size = f->off + len;
        memcpy(f->inode, cur->block.din, sizeof(struct sfs_inode));
    }
    f->off += len;

    return len;
}

int sfs_get_files(const char* path, char* files[]){
    if(!fs)sfs_init();
    int size = strsize(path), lst=1, num=0;
    if(path[0]!='/'){
        printf("Invalid path\n");
        return -1;
    }
    Mblock cur = find_block(1, DIN);
    for(int i = 1; i < size; i++){
        if(path[i]=='/'){
            int len = i - lst;
            char* name = (char*)kmalloc(len);
//            Mem_used += len;
            memcpy(name, path[lst], len);
            name[len]='\0';
            cur = find_file(name, cur);
            if(!cur)return -1;
            lst = i+1;
        }
    }
    int len = size - lst;
    if(len){
        char* name = (char*)kmalloc(len);
//        Mem_used += len;
        memcpy(name, path[lst], len);
        name[len]='\0';
        cur = find_file(name, cur);
        if(!cur)return -1;
    }
    if(cur->block.din->type){
        while(cur->block.din->indirect){
            for(int i=0;i<cur->block.din->blocks;i++){
                Mblock mem = find_block(cur->block.din->direct[i], DEN);
                int s = strsize(mem->block.den->filename);
                files[num] = (char*)kmalloc(s);
//                Mem_used += s;
                memcpy(files[num], mem->block.den->filename, s);
                num++;
            }
            cur = find_block(cur->block.din->indirect, DIN);
        }
        for(int i=0;i<cur->block.din->blocks;i++){
            Mblock mem = find_block(cur->block.din->direct[i], DEN);
            int s = strsize(mem->block.den->filename);
            memcpy(files[num], mem->block.den->filename, s);
            num++;
        }
        return num;
    }
    else{
        return 0;
    }
}