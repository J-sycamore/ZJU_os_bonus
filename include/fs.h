#pragma once

#include "defs.h"

#define SFS_MAX_INFO_LEN     32
#define SFS_MAGIC            0x1f2f3f4f
#define SFS_NDIRECT          11
#define SFS_DIRECTORY        1
#define SFS_MAX_FILENAME_LEN 27

#define SEEK_CUR 0
#define SEEK_SET 1
#define SEEK_END 2

#define SFS_FILE 0
#define SFS_DIRECTORY 1

#define SFS_FLAG_READ (0x1)
#define SFS_FLAG_WRITE (0x2)

#define min(a,b) (a>b?b:a)

struct sfs_super {
    uint32_t magic;
    uint32_t blocks;
    uint32_t unused_blocks;
    char info[SFS_MAX_INFO_LEN + 1];
};

struct sfs_inode {
    uint32_t size;                 // 文件大小
    uint16_t type;                 // 文件类型，文件/目录
    uint16_t links;                // 硬链接数量
    uint32_t blocks;               // 本文件占用的 block 数量
    uint32_t direct[SFS_NDIRECT];  // 直接数据块的索引值
    uint32_t indirect;             // 间接索引块的索引值
};

struct sfs_entry {
    uint32_t ino;                            // 文件的 inode 编号
    char filename[SFS_MAX_FILENAME_LEN + 1]; // 文件名
};



/**
 * 功能: 初始化 simple file system
 * @ret : 成功初始化返回 0，否则返回非 0 值
 */
int sfs_init();


/**
 * 功能: 打开一个文件, 读权限下如果找不到文件，则返回一个小于 0 的值，表示出错，写权限如果没有找到文件，则创建该文件（包括缺失路径）
 * @path : 文件路径 (绝对路径)
 * @flags: 读写权限 (read, write, read | write)
 * @ret  : file descriptor (fd), 每个进程根据 fd 来唯一的定位到其一个打开的文件
 *         正常返回一个大于 0 的 fd 值, 其他情况表示出错
 */
int sfs_open(const char* path, uint32_t flags);


/**
 * 功能: 关闭一个文件，并将其修改过的内容写回磁盘
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @ret : 正确关闭返回 0, 其他情况表示出错
 */
int sfs_close(int fd);


/**
 * 功能  : 根据 fromwhere + off 偏移量来移动文件指针(可参考 C 语言的 fseek 函数功能)
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @off : 偏移量
 * @fromwhere : SEEK_SET(文件头), SEEK_CUR(当前), SEEK_END(文件尾)
 * @ret : 表示错误码
 *        = 0 正确返回
 *        < 0 出错
 */
int sfs_seek(int fd, int32_t off, int fromwhere);


/**
 * 功能  : 从文件的文件指针开始读取 len 个字节到 buf 数组中 (结合 sfs_seek 函数使用)，并移动对应的文件指针
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @buf : 读取内容的缓存区
 * @len : 要读取的字节的数量
 * @ret : 返回实际读取的字节的个数
 *        < 0 表示出错
 *        = 0 表示已经到了文件末尾，没有能读取的了
 *        > 0 表示实际读取的字节的个数，比如 len = 8，但是文件只剩 5 个字节的情况，就是返回 5
 */
int sfs_read(int fd, char* buf, uint32_t len);


/**
 * 功能  : 把 buf 数组的前 len 个字节写入到文件的文件指针位置(覆盖)(结合 sfs_seek 函数使用)，并移动对应的文件指针
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @buf : 写入内容的缓存区
 * @len : 要写入的字节的数量
 * @ret : 返回实际的字节的个数
 *        < 0 表示出错
 *        >=0 表示实际写入的字节数量
 */
int sfs_write(int fd, char* buf, uint32_t len);


/**
 * 功能    : 获取 path 下的所有文件名，并存储在 files 数组中
 * @path  : 文件夹路径 (绝对路径)
 * @files : 保存该文件夹下所有的文件名
 * @ret   : > 0 表示该文件夹下有多少文件
 *          = 0 表示该 path 是一个文件
 *          < 0 表示出错
 */
int sfs_get_files(const char* path, char* files[]);

// -------------------------------------------------------
// ------------ 以下数据结构和缓存设计可自行修改---------------

typedef int KIND;
#define BLOCK 0
#define DIN 1
#define DEN 2

// 内存中的 block 缓存结构
struct sfs_memory_block {
     union {
         struct sfs_inode* din;   // 可能是 inode 块
         struct sfs_entry* den;
         char *block;      // 可能是数据块
     } block;
     KIND kind;        // 是否是 inode 0:block 1:din 2:den
     uint32_t blockno;     // block 编号
     bool dirty;           // 脏位，保证写回数据
     int reclaim_count;    // 指向次数，因为硬链接有可能会打开同一个 inode，所以需要记录次数
};

typedef struct sfs_super* SUPER;
typedef struct sfs_inode* INODE;
typedef struct sfs_entry* ENTRY;
typedef struct sfs_memory_block* Mblock;

struct sfs_fs {
    struct sfs_super super;           // SFS 的超级块
    char freemap[4096];           // freemap 区域管理，可自行设计
    bool super_dirty;          // 超级块或 freemap 区域是否有修改
    Mblock hash[256];
    uint32_t size;
};

int streql(char* a, char* b);

int strsize(char* a);

Mblock inode_to_mem(INODE din, int no);

Mblock entry_to_mem(ENTRY den, int no);

Mblock data_to_mem(char* data, int no);

void add_cache(Mblock mem);

int write_back(Mblock mem);

uint32_t find_freeblock();

Mblock find_block(uint32_t num, uint16_t type);

Mblock find_file(char* name, Mblock dir);

Mblock create_entry(char* name, uint32_t no);

Mblock create_inode(uint16_t type);

Mblock create_data();

Mblock create_dir(char* name, Mblock dir);

Mblock create_file(char* name, Mblock dir);