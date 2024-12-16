#pragma once
#ifndef __NGX_MEM_POOL_H__
#define __NGX_MEM_POOL_H__

/*
    try to implement a nxg_mem_pool
    include basic memory pool create, destory, cb function, allocate
    1 paper = 4k
    default allocate = 16k
                    ngx memeory pool
                    |---------------
                    |   last    | point to own block's free memory start addr
                    |-----------
                    |   end     | point to own block's end memory end addr
                    |----------- pool_data_t
                    |   nxt     | point to nxt block
                    |-----------
                    |   failed  | own block allocate memory fail times
                    |---------------
                    |   max     | tag to distinguishing small/big memory
                    |-----------
                    |   current | point to avaliable allocate small memory's pool
                    |-----------                                                        
                    |   large   | point to big memory msg----------------------------->|  nxt    |
                    |-----------                                                       |---------
                    |   cleanUp | point to cb msg ----------->| nxt |                  |  malloc |
                    |-----------|                             |-----|
                                                              | cb  |  
                                                              |-----|
                                                              | data| 
*/
//configure
using u_char = unsigned char;
using u_int = unsigned int;
struct ngx_pool_s;
struct ngx_large_data_s;

enum NGX_FLAG {
    NGX_OK,         // 0
    NGX_DECLIEND    // 1
};

//align
#define ngx_align(d,a) (((d) + (a - 1)) & ~(a - 1))
#define ngx_align_ptr(p, a) \
(u_char*)(((uintptr_t)(p)+((uintptr_t)a - 1)) & ~((uintptr_t)a - 1))

const u_int NGX_PAGESIZE = 4096;
const u_int NGX_DEFAULT_POOL_SIZE = (16 * 1024);    //16K
const u_int NGX_POOL_ALIGNMENT = 16;

// block data
struct ngx_pool_data_s {
    u_char* last;
    u_char* end;
    ngx_pool_s* nxt;
    u_int failed;
};
// large memory data
struct ngx_large_data_s {
    void* _alloc;
    ngx_large_data_s* _nxt;
};
//call back data
typedef void (*handler)(void*);
struct clean_up_s {
    handler _cb;
    void* _data;
    clean_up_s* nxt;
};

//pool data
struct ngx_pool_s{
    ngx_pool_data_s _d;
    size_t _max;
    ngx_pool_s* _current;
    ngx_large_data_s* _large;
    clean_up_s* _clean_up;
};

const u_int NGX_MIN_POOL_SIZE = ngx_align((sizeof(ngx_pool_s) + 2 * sizeof(ngx_large_data_s), NGX_POOL_ALIGNMENT));

//oop to ngx memory pool
class ngx_mem_pool {
public:
    //create size bytes memory pool
    void* ngx_pool_create(size_t size);
    
    //allocate memory
    //consider memory align
    void* ngx_palloc(size_t size);
    //unconsider memory align
    void* ngx_pnalloc(size_t size);
    //init allocate's memory to 0
    void* ngx_pcalloc(size_t size);

    //release memory, just consider big memory
    u_int ngx_pfree(void* p);

    //reset memory pool
    u_int ngx_reset_pool();

    //destory memory pool
    u_int ngx_destory_pool();

    //add cb, in order to cleanup extern resources
    clean_up_s* ngx_cleanup_add(size_t size);

private:
    ngx_pool_s* _pool;

    void* ngx_palloc_small(size_t size, u_int align);
    void* ngx_palloc_large(size_t size);
    void* ngx_palloc_block(size_t size);
};

#include <cstdio>
#include <memory>
namespace ngx_mem_pool_test {
    typedef struct Data stData;
    struct Data {
        char* ptr;
        FILE* pfile;
    };
    static void func1(void* p1) {
        char* p = (char*)p1;
        printf("free ptr mem!");
        free(p);
    }
    static void func2(void* pf1) {
        FILE* pf = (FILE*)pf1;
        printf("close file!");
        fclose(pf);
    }
    static void test01() {
        ngx_mem_pool pool;
        if (!pool.ngx_pool_create(512))
            throw "memory pool creat failed!";

        void* p1 = pool.ngx_palloc(128);
        if (!p1) throw "allocate p1 failed!";

        stData* p2 = (stData*)pool.ngx_palloc(512);
        if (!p2) throw "allocate p2 failed!";
        p2->ptr = (char*)malloc(12);
        strcpy(p2->ptr, "hello world");
        p2->pfile = fopen("data.txt", "w");

        clean_up_s* c1 = pool.ngx_cleanup_add(sizeof(char*));
        c1->_cb = func1;
        c1->_data = p2->ptr;

        clean_up_s* c2 = pool.ngx_cleanup_add(sizeof(FILE*));
        c2->_cb = func2;
        c2->_data = p2->pfile;

        pool.ngx_destory_pool();
    }
}

#endif // !__NGX_MEM_POOL_H__
