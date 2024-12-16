#include "Header/ngx_mem_pool.h"

/*
public function
*/
#include <cstdlib>
void* ngx_mem_pool::ngx_pool_create(size_t size) {
    //size shoubl be bigger than sizeof(pool)

    _pool = (ngx_pool_s *)malloc(size); // malloc default align to 16bytes in x86-64
    if (!_pool) return nullptr;

    //init pool
    _pool->_d.last = (u_char *)_pool + sizeof(ngx_pool_s);
    _pool->_d.end = (u_char *)_pool + size;
    _pool->_d.nxt = nullptr;
    _pool->_d.failed = 0;

    _pool->_current = _pool;
    size -= sizeof(ngx_pool_s);
    _pool->_max = size < NGX_PAGESIZE ? size : NGX_PAGESIZE;
    _pool->_large = nullptr;
    _pool->_clean_up = nullptr;

    void* p = (void *)_pool;
    return p;
}

void* ngx_mem_pool::ngx_palloc(size_t size) {
    return size <= _pool->_max ? ngx_palloc_small(size, 1) :
        ngx_palloc_large(size);
}

void* ngx_mem_pool::ngx_pnalloc(size_t size) {
    return size <= _pool->_max ? ngx_palloc_small(size, 0) :
        ngx_palloc_large(size);
}

#include <cstring>
void* ngx_mem_pool::ngx_pcalloc(size_t size) {
    void* p = ngx_palloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

u_int ngx_mem_pool::ngx_pfree(void* p) {
    ngx_large_data_s* l;
    for (l = _pool->_large; l; l = l->_nxt) {
        if (p == l->_alloc) {
            free(l->_alloc);
            l->_alloc = nullptr;
            return NGX_OK;
        }
    }
    return NGX_DECLIEND;
}

u_int ngx_mem_pool::ngx_reset_pool() {
    //befor calling reset(), l->_alloc's extern resources should be free
    ngx_large_data_s* l;
    for (l = _pool->_large; l; l = l->_nxt) {
        if (l->_alloc) {
            free(l->_alloc);
            l->_alloc = nullptr;
        }
    }

    ngx_pool_s* p = _pool;
    p->_d.last = (u_char *)_pool + sizeof(ngx_pool_s);
    p->_d.failed = 0;

    for (p = p->_d.nxt; p; p = p->_d.nxt) {
        p->_d.last = (u_char *)p + sizeof(ngx_pool_data_s);
        p->_d.failed = 0;
    }

    _pool->_current = _pool;
    _pool->_large = nullptr;
    return NGX_OK;
}

//destory memory pool
u_int ngx_mem_pool::ngx_destory_pool() {
    for (clean_up_s* c = _pool->_clean_up; c;
        c = c->nxt) {
        if (c->_cb) {
            c->_cb(c->_data);
        }
    }

    for (ngx_large_data_s* l = _pool->_large; l;
        l = l->_nxt) {
        if (l->_alloc)
            free(l->_alloc);
    }

    ngx_pool_s* p = _pool;
    ngx_pool_s* n = p->_d.nxt;
    for (;; p = n, n = n->_d.nxt) {
        if (p)
            free(p);
        if (!n)
            break;
    }

    return NGX_OK;
}

//add cb, in order to cleanup extern resources
clean_up_s* ngx_mem_pool::ngx_cleanup_add(size_t size) {
    clean_up_s* p = (clean_up_s *)ngx_palloc(sizeof(clean_up_s));
    if (!p) return nullptr;

    //size is useful??
    if (size) {
        p->_data = ngx_palloc(size);
        if (!p->_data) return nullptr;
    }
    else
        p->_data = nullptr;

    p->_cb = nullptr;
    p->nxt = _pool->_clean_up;
    _pool->_clean_up = p;

    return p;
}

/*
private function
*/
void* ngx_mem_pool::ngx_palloc_small(size_t size, u_int align) {
    ngx_pool_s* p = _pool->_current;
    u_char* m;

    do {
        m = p->_d.last;
        if (align) m = ngx_align_ptr(m, NGX_POOL_ALIGNMENT);
        if ((size_t)(p->_d.end - m) >= size) {
            p->_d.last = m + size;
            return m;
        }
        p = p->_d.nxt;
    } while (p);

    return ngx_palloc_block(size);
}

void* ngx_mem_pool::ngx_palloc_block(size_t size) {

    size_t block_size = (size_t)(_pool->_d.end
        - (u_char*)_pool);
    u_char* m = (u_char *)malloc(block_size);
    if (!m) return nullptr;

    ngx_pool_s* block = (ngx_pool_s *)m;
    block->_d.end = m + block_size;
    block->_d.failed = 0;
    block->_d.nxt = nullptr;

    m += sizeof(ngx_pool_data_s);
    m = ngx_align_ptr(m, NGX_POOL_ALIGNMENT);
    block->_d.last = m + size;

    ngx_pool_s* p = _pool->_current;
    for (; p->_d.nxt; p = p->_d.nxt) {
        if (p->_d.failed++ > 4)
            _pool->_current = p->_d.nxt;
    }
    p->_d.nxt = block;

    return m;
}

void* ngx_mem_pool::ngx_palloc_large(size_t size) {
    void* p = malloc(size);
    if (!p) return nullptr;
    
    u_int n = 0;
    for (ngx_large_data_s* l = _pool->_large; l; l = l->_nxt) {
        if (!l->_alloc) {
            l->_alloc = p;
            return p;
        }
        if (n++ > 3)
            break;
    }

    ngx_large_data_s* l = (ngx_large_data_s *)ngx_palloc(sizeof(ngx_large_data_s));
    if (!l) { free(p); return nullptr; }

    l->_nxt = _pool->_large;
    l->_alloc = p;
    _pool->_large = l;

    return p;
}