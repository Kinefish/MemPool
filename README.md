# ngx内存分配

## ngx小块内存池分配

`ngx_palloc_small`

>从`pool->current`指向的内存块开始分配内存，只有第一次通过`ngx_creat_pool()`分配的内存块才带有`struct ngx_pool_s`中除`struct ngx_pool_data_t`以外的信息

- 将`pool`指向`current`
- 从`pool->last`指向的内存地址`m`开始分配，如果需要内存对齐，则将`m`对齐
- 每一块`block`通过`pool->last`以及`pool->end`来记录可分配的空闲内存
- 如果剩余内存可以进行这次的分配，则更新`pool->last`后，将`m`返回；否则先进入下一块`block`
- 如果当前所有的`block`的空闲内存都不足够分配，则分配新的`block`

```cpp
static ngx_inline void *
ngx_palloc_small(ngx_pool_t *pool, size_t size, ngx_uint_t align)
{
    u_char      *m;
    ngx_pool_t  *p;

    p = pool->current;

    do {
        m = p->d.last;

        if (align) {
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
        }

        if ((size_t) (p->d.end - m) >= size) {
            p->d.last = m + size;

            return m;
        }

        p = p->d.next;

    } while (p);

    return ngx_palloc_block(pool, size);
}
```



`ngx_palloc_block`

> ​	完成`new block`的分配，`current`的更新

- 每一个`block`的大小由`pool->d.end - (u_char* pool);`进行控制，也就是将要分配新的`block`的大小
- 实际上新的`block`最终由`malloc`调用，并返回`&new block`
- `new block`只负责记录`struct ngx_pool_data_t`数据，并更新
- 在`new block`根据是否需要内存确定好`m`后，更新`new block->d.last`，以确保下一次来到这个`block`后，找得到新的空闲内存地址
- 将`new block`挂到上一个无法完成内存分配的`block->d.nxt`中
- 最后从`current`指针开始遍历所有分配失败的`block`，记录失败次数，如果每一块`block`分配失败超过`4`次，默认该`block`剩余的空闲内存过小，更新`urrent`，以便下一次的内存分配从新的`current`开始

```cpp
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new;

    psize = (size_t) (pool->d.end - (u_char *) pool);

    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    if (m == NULL) {
        return NULL;
    }

    new = (ngx_pool_t *) m;

    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    m += sizeof(ngx_pool_data_t);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    new->d.last = m + size;

    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {
            pool->current = p->d.next;
        }
    }

    p->d.next = new;

    return m;
}
```

