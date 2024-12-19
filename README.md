# SGISTL内存管理

> 在频繁使用小块内存是分配、释放下，很容易产生内存碎片，并且`malloc`出的内存同样带有`cookie`
>
> `sgi stl`使用的容器空间配置器和`gnu 2.91`的源码一样，都是使用`align = 8`的自由链表来进行内存空间的分配和释放。
>
> 貌似是线程安全的

## 对齐函数

> 两个关键的参数
>
> A：`__bytes + (size_t)_ALIGN - 1`
>
> B：`(size_t)_ALIGN - 1`

`_S_round_up`

- 通过位运算将`bytes`向上取整`_ALIGN`的倍数

~~~cpp
static size_t _S_round_up(size_t __bytes) {
    return A & (~ B);
}
~~~

`_S_freelist_index`

- 取`__bytes`在`freelist`中的`index`

```cpp
static size_t _S_freelist_index(size_t __bytes) {
    return A / B;
}
```

## 内存分配

> 通过维护`_S_free_list`来分配不同的`chunk`

`static void* allocate(size_t __n)`

- 如果`__n`超过`128bytes`，则通过`malloc`进行分配

- 先定位`idx`
- 如果对应`idx`有空闲`chunk`，将`chunk`返回，同时更新对应`idx`的空闲`chunk`；反之，初始化一组`chunk`（链表形式），再返回第一块空闲`chunk`

```cpp
/* __n must be > 0      */
  static void* allocate(size_t __n)
  {
    void* __ret = 0;
	
    //_MAX_BYTES 128 bytes
    if (__n > (size_t) _MAX_BYTES) {
      __ret = malloc_alloc::allocate(__n);
    }
    else {
      _Obj* __STL_VOLATILE* __my_free_list
          = _S_free_list + _S_freelist_index(__n);
      // Acquire the lock here with a constructor call.
      // This ensures that it is released in exit or during stack
      // unwinding.
#     ifndef _NOTHREADS
      /*REFERENCED*/
      _Lock __lock_instance;
#     endif
      _Obj* __RESTRICT __result = *__my_free_list;
      if (__result == 0)
        __ret = _S_refill(_S_round_up(__n));
      else {
        *__my_free_list = __result -> _M_free_list_link;
        __ret = __result;
      }
    }

    return __ret;
  };
```

## 初始化chunk

> 负责将分配好的初始化内存以静态链表的形式连起来，使用静态链表的形式可以更极致的使用内存

`_S_refill`

- 每个`chunk`占`__n bytes`

- 初始化的内存`chunk`通过`_S_chunk_alloc()`返回
- chunk的大小由`nobjs`决定
- 只负责将每个`chunk`连接

```cpp
void*
__default_alloc_template<__threads, __inst>::_S_refill(size_t __n)
{
    int __nobjs = 20;
    char* __chunk = _S_chunk_alloc(__n, __nobjs);
    _Obj* __STL_VOLATILE* __my_free_list;
    _Obj* __result;
    _Obj* __current_obj;
    _Obj* __next_obj;
    int __i;

    if (1 == __nobjs) return(__chunk);
    __my_free_list = _S_free_list + _S_freelist_index(__n);

    /* Build free list in chunk */
      __result = (_Obj*)__chunk;
      *__my_free_list = __next_obj = (_Obj*)(__chunk + __n);
      for (__i = 1; ; __i++) {
        __current_obj = __next_obj;
        __next_obj = (_Obj*)((char*)__next_obj + __n);
        if (__nobjs - 1 == __i) {
            __current_obj -> _M_free_list_link = 0;
            break;
        } else {
            __current_obj -> _M_free_list_link = __next_obj;
        }
      }
    return(__result);
}
```

## 分配chunk

> 通过递归调用，返回`_S_refill()`需要的起始`chunk`地址，并且会修改`__nobjs`
>
> 通过`_S_start_free`、`_S_end_free`、`_S_heap_size`决定分配`chunk`

### 正常情况

- 初始化情况
- 通过`malloc`分配两倍的`chunks bytes`，默认分配`20`个`chunk`，剩下`20`个`chunk`作为备用。前`20`个`chunk`在`_S_refill()`中连接起来

### 备用情况

- 备用内存的`bytes`通过`_S_end_free - _S_start_free`决定
- 备用内存可以分割成不同大小的`chunk`
- 会改变`nobjs`
- 当备用内存不够分配新的`chunk`时，会重新走初始化`chunk`，但是如果`bytes_left > 0`的话，会在初始化前先将剩余内存（头插法）挂到对应的`idx`下

### 失败情况

> 每次初始化`chunk`都会`malloc`较大的内存空间，所以有可能失败

- 优先查看`idx`后的`_S_free_list`是否由空闲的`chunk`
- 进入`while(1)`，如果有自己写好的资源释放函数，调用定义好的`handler`释放内存，直到有内存释放为止
- 如果没有，直接`BAD_ALLOC`异常

`_S_chunk_alloc`

```cpp
char*
__default_alloc_template<__threads, __inst>::_S_chunk_alloc(size_t __size, 
                                                            int& __nobjs)
{
    char* __result;
    size_t __total_bytes = __size * __nobjs;
    size_t __bytes_left = _S_end_free - _S_start_free;

    if (__bytes_left >= __total_bytes) {//一般是正常情况
        __result = _S_start_free;
        _S_start_free += __total_bytes;
        return(__result);
    } else if (__bytes_left >= __size) {//备用内存的分配
        __nobjs = (int)(__bytes_left/__size);
        __total_bytes = __size * __nobjs;
        __result = _S_start_free;
        _S_start_free += __total_bytes;
        return(__result);
    } else {//初始化情况，会同时判断剩余bytes和malloc失败的情况
        size_t __bytes_to_get = 
	  2 * __total_bytes + _S_round_up(_S_heap_size >> 4);
        // Try to make use of the left-over piece.
        if (__bytes_left > 0) {//剩余字节不够分配新的chunk
            _Obj* __STL_VOLATILE* __my_free_list =
                        _S_free_list + _S_freelist_index(__bytes_left);

            ((_Obj*)_S_start_free) -> _M_free_list_link = *__my_free_list;
            *__my_free_list = (_Obj*)_S_start_free;
        }
        _S_start_free = (char*)malloc(__bytes_to_get);
        if (0 == _S_start_free) {
            size_t __i;
            _Obj* __STL_VOLATILE* __my_free_list;
	    _Obj* __p;
            // Try to make do with what we have.  That can't
            // hurt.  We do not try smaller requests, since that tends
            // to result in disaster on multi-process machines.
            for (__i = __size;//找出空余的chunk，分配给当前
                 __i <= (size_t) _MAX_BYTES;
                 __i += (size_t) _ALIGN) {
                __my_free_list = _S_free_list + _S_freelist_index(__i);
                __p = *__my_free_list;
                if (0 != __p) {//找到了空余的chunk
                    *__my_free_list = __p -> _M_free_list_link;
                    _S_start_free = (char*)__p;
                    _S_end_free = _S_start_free + __i;
                    return(_S_chunk_alloc(__size, __nobjs));
                    // Any leftover piece will eventually make it to the
                    // right free list.
                }
            }//endif for，找不出空余的chunk
	    _S_end_free = 0;	// In case of exception.
            _S_start_free = (char*)malloc_alloc::allocate(__bytes_to_get);//调用handler/抛出异常
            // This should either throw an
            // exception or remedy the situation.  Thus we assume it
            // succeeded.
        }
        _S_heap_size += __bytes_to_get;
        _S_end_free = _S_start_free + __bytes_to_get;
        return(_S_chunk_alloc(__size, __nobjs));//不同情况的递归调用
    }
}
```

## 内存释放

`deallocate()`

- 如果是通过`malloc`分配的内存，通过`free`释放
- 因为返还的内存可能没有顺序，所以设计成静态链表会更方便
- 直接头插法挂在`_S_free_list`上就好

```cpp
/* __p may not be 0 */
  static void deallocate(void* __p, size_t __n)
  {
    if (__n > (size_t) _MAX_BYTES)
      malloc_alloc::deallocate(__p, __n);
    else {
      _Obj* __STL_VOLATILE*  __my_free_list
          = _S_free_list + _S_freelist_index(__n);
      _Obj* __q = (_Obj*)__p;

      // acquire lock
#       ifndef _NOTHREADS
      /*REFERENCED*/
      _Lock __lock_instance;
#       endif /* _NOTHREADS */
      __q -> _M_free_list_link = *__my_free_list;
      *__my_free_list = __q;
      // lock is released here
    }
  }
```



# ngx内存管理

> `ngx`将`4k`作为大小块内存分配的界限

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
- 如果想要通过内存池自动管理外部资源的话，可以传相应的`size`，这样内存池就会开辟相应的`size`，可以通过强转利用这个`size`

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



