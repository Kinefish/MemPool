# ngx内存管理

> ngx将`4k`作为大小块内存分配的界限

## 小块内存池分配

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

## 大块内存池分配

`ngx_palloc_large`

> 分配的内存、分配头信息的内存	

- 由`malloc`分配大块内存`p`
- 同时调用`ngx_palloc_small`来分配存储大块内存头信息`typedef xxx ngx_pool_large_t`的内存，意味着信息存在`block`中
- 用**头插法**将每一个新生成的大块内存的信息插入`pool->large`中
- 将`p`记录到信息中，如果链表的前`3`个头信息都挂有大内存的地址，就不再遍历了，而是从`block`中生成新的头信息内存，再将这个新的`p`挂到上面

```cpp
static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) {
            break;
        }
    }

    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}

```

## 大块内存释放

> 只释放大内存，不释放小内存，跟场景有关

- 大内存由`malloc`开辟，信息存在`block`中，以便释放的时候能找到
- 大内存由`free`释放

```cpp
ngx_int_t
ngx_pfree(ngx_pool_t *pool, void *p)
{
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "free: %p", l->alloc);
            ngx_free(l->alloc);
            l->alloc = NULL;

            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}
```

## 内存池重置

> `ngx`的是一个支持短连接的`http`服务器，当与`client`的连接断开时，可以重置此时的内存池，并等待下一次的连接

`ngx_reset_pool`

- 大块内存调用`free`，大块内存的头信息记录在`block`中
- 小块内存`block`通过`p->last`的复位进行重置

```cpp
void
ngx_reset_pool(ngx_pool_t *pool)
{
    ngx_pool_t        *p;
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    /*block的重置会浪费一部分空间，因为只有第一个内存池才带有
    完整的ngx_pool_t信息，其余的block只有ngx_pool_data_t
    */
    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(ngx_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->chain = NULL;
    pool->large = NULL;
}
```

## 外部资源的释放

> 利用`cb`进行外部资源释放，如果是文件，提供了文件的`cleanup`调用

`ngx_pool_cleanup_add`

- 外部资源头内存同样在`block`中申请
- 每个头信息用头插法串起来，最后挂在`pool->cleanup`上
- `cb`和`size`存入头信息中

```cpp
ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)
{
    ngx_pool_cleanup_t  *c;

    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size);
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }

    c->handler = NULL;
    c->next = p->cleanup;

    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}
```

## 内存池销毁

> 先释放外部资源，再释放大块内存，最后是`block`的释放

`ngx_destroy_pool`

- 先遍历`pool->cleanup`，调用`cb(data)`释放外部资源
- 再遍历`pool->large`，释放大块内存
- 最后遍历`pool->d.nxt`，释放每个`block`

```cpp
void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    for (c = pool->cleanup; c; c = c->next) {
        if (c->handler) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);
        }
    }

    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}
```



