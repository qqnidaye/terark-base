#pragma once
#include "valvec.hpp"
#include <terark/util/atomic.hpp>
#include <terark/util/throw.hpp>
#include <stdexcept>
#include <boost/integer/static_log2.hpp>
#include <boost/mpl/if.hpp>
#include <mutex>
#include "mempool_lock_free.hpp"
#include "mempool_lock_none.hpp"
#include "mempool_fixed_cap.hpp"
#include "mempool_lock_mutex.hpp"

namespace terark {

template<int AlignSize>
class MemPool_CompileX : private valvec<unsigned char> {
    size_t  fragment_size; // for compatible with MemPool_Lock(Free|None|Mutex)
    typedef valvec<unsigned char> mem;
public:
    using mem::data;
    using mem::size; // bring to public...
    using mem::reserve;
    using mem::capacity;
    using mem::risk_set_data;
    using mem::risk_release_ownership;
    size_t free_size() const { return fragment_size; }
//  void   sfree(size_t,size_t) { assert(false); }
//  size_t alloc(size_t) { assert(false); return 0; }
//  size_t alloc3(size_t,size_t,size_t) { assert(false); return 0; }
    valvec<unsigned char>* get_valvec() { return this; }
};

/// mempool which alloc mem block identified by
/// integer offset(relative address), not pointers(absolute address)
/// integer offset could be 32bit even in 64bit hardware.
///
/// the returned offset is aligned to align_size, this allows 32bit
/// integer could address up to 4G*align_size memory
///
/// when memory exhausted, valvec can realloc memory without memcpy
/// @see valvec
#define MemPool_ThisType MemPool
template<int AlignSize>
class MemPool_ThisType : private valvec<unsigned char> {
    BOOST_STATIC_ASSERT((AlignSize & (AlignSize-1)) == 0);
    BOOST_STATIC_ASSERT(AlignSize >= 4);
    typedef valvec<unsigned char> mem;
    typedef typename boost::mpl::if_c<AlignSize == 4, uint32_t, uint64_t>::type link_size_t;

    static const size_t skip_list_level_max = 8;    // data io depend on this, don't modify this value
    static const size_t list_tail = ~link_size_t(0);
    static const size_t offset_shift = AlignSize == 4 ? boost::static_log2<AlignSize>::value : 0;

    typedef link_size_t link_t;
    struct huge_link_t {
        link_size_t size;
        link_size_t next[skip_list_level_max];
    };
    struct LockFreeHead {
        link_size_t head;
        link_size_t cnt;
        char   padding[64 - 2*sizeof(link_size_t)]; // avoid false sharing
        LockFreeHead() {
            head = link_size_t(list_tail);
            cnt = 0;
            memset(padding, 0, sizeof(padding));
        }
    };
    BOOST_STATIC_ASSERT(sizeof(LockFreeHead) == 64);
    struct head_t {
        head_t() : head(list_tail), cnt(0) {}
        link_size_t head;
        link_size_t cnt;
    };
    size_t  fragment_size; // for compatible with MemPool_Lock(Free|None|Mutex)
    size_t  huge_size_sum;
    size_t  huge_node_cnt;
    huge_link_t huge_list; // huge_list.size is max height of skiplist
    head_t* free_list_arr;
    size_t  free_list_len;
    std::mutex  huge_mutex;
    std::mutex  update_n_mutex;
    valvec<std::mutex> free_list_mutex;
    valvec<LockFreeHead> free_list_lock_free;

    size_t random_level() {
        size_t level = 1;
        while (rand() % 4 == 0 && level < skip_list_level_max)
            ++level;
        return level - 1;
    }

public:
    // low level method
    void destroy_and_clean() {
        mem::clear();
        clear_free_list();
    }

    // low level method
    void clear_free_list() {
        if (free_list_arr) {
            free(free_list_arr);
            free_list_arr = NULL;
        }
        free_list_mutex.clear();
        free_list_lock_free.clear();
        free_list_len = 0;
        fragment_size = 0;
        huge_size_sum = 0;
        huge_node_cnt = 0;
        huge_list.size = 0;
        for(auto& next : huge_list.next)
            next = list_tail;
    }

public:
    enum LockType {
        LockNone,
        LockFree,
        LockMutex,
    };
          mem& get_data_byte_vec()       { return *this; }
    const mem& get_data_byte_vec() const { return *this; }

    static size_t align_to(size_t len) {
        return (len + align_size - 1) & ~size_t(align_size - 1);
    }
    enum { align_size = AlignSize };

    explicit MemPool_ThisType(size_t maxBlockSize) {
        assert(maxBlockSize >= align_size);
        assert(maxBlockSize >= sizeof(huge_link_t));
        maxBlockSize = align_to(maxBlockSize);
        free_list_len = maxBlockSize / align_size;
        free_list_arr = (head_t*)malloc(sizeof(head_t) * free_list_len);
        if (NULL == free_list_arr) {
            throw std::bad_alloc();
        }
        fragment_size = 0;
        huge_size_sum = 0;
        huge_node_cnt = 0;
        std::uninitialized_fill_n(free_list_arr, free_list_len, head_t());
        huge_list.size = 0;
        for(auto& next : huge_list.next) next = list_tail;
    }
    MemPool_ThisType(const MemPool_ThisType& y) : mem(y) {
        free_list_len = y.free_list_len;
        free_list_arr = (head_t*)malloc(sizeof(head_t) * free_list_len);
        if (NULL == free_list_arr) {
            throw std::bad_alloc();
        }
        fragment_size = y.fragment_size;
        huge_size_sum = y.huge_size_sum;
        huge_node_cnt = y.huge_node_cnt;
        memcpy(free_list_arr, y.free_list_arr, sizeof(head_t) * free_list_len);
        free_list_lock_free = y.free_list_lock_free;
        free_list_mutex.resize(free_list_len);
        huge_list = y.huge_list;
    }
    MemPool_ThisType& operator=(const MemPool_ThisType& y) {
        if (&y == this)
            return *this;
        destroy_and_clean();
        MemPool_ThisType(y).swap(*this);
        return *this;
    }
    ~MemPool_ThisType() {
        if (free_list_arr) {
            free(free_list_arr);
            free_list_arr = NULL;
        }
    }

#ifdef HSM_HAS_MOVE
    MemPool_ThisType(MemPool_ThisType&& y) noexcept
        : mem(std::move(y))
        , free_list_lock_free(std::move(y.free_list_lock_free)) {
        assert(y.data() == NULL);
        assert(y.size() == 0);
        free_list_len = y.free_list_len;
        free_list_arr = y.free_list_arr;
        fragment_size = y.fragment_size;
        huge_size_sum = y.huge_size_sum;
        huge_node_cnt = y.huge_node_cnt;
        huge_list = y.huge_list;
        y.free_list_len = 0;
        y.free_list_arr = NULL;
        y.fragment_size = 0;
        y.huge_size_sum = 0;
        y.huge_node_cnt = 0;
        y.huge_list.size = 0;
        for(auto& next : y.huge_list.next) next = list_tail;
    }
    MemPool_ThisType& operator=(MemPool_ThisType&& y) noexcept {
        if (&y == this)
            return *this;
        this->~MemPool_ThisType();
        ::new(this) MemPool_ThisType(std::move(y));
        return *this;
    }
#endif

    void get_fastbin(valvec<size_t>* fast) const {
        fast->resize_fill(free_list_len, 0);
        size_t* ptr = fast->data();
        if (free_list_lock_free.size()) {
            LockFreeHead* arr = free_list_lock_free.data();
            size_t  len = free_list_lock_free.size();
            for (size_t i = 0; i < len; ++i)
                ptr[i] = arr[i].cnt.load(std::memory_order_relaxed);
        }
        else if (free_list_mutex.size()) {
            head_t* arr = free_list_arr;
            size_t  len = free_list_len;
            for (size_t i = 0; i < len; ++i)
                ptr[i] = arr[i].cnt;
        }
    }

    size_t get_huge_stat(size_t* huge_memsize) const {
        *huge_memsize = huge_size_sum;
        return huge_node_cnt;
    }

    using mem::data;
    using mem::size; // bring to public...
    using mem::reserve;
    using mem::capacity;
    using mem::risk_release_ownership;

    void risk_set_data(const void* data, size_t len) {
        assert(NULL == mem::p);
        assert(0 == mem::n);
        assert(0 == mem::c);
        mem::risk_set_data((unsigned char*)data, len);
        clear_free_list();
    }

    unsigned char byte_at(size_t pos) const {
        assert(pos < n);
        return p[pos];
    }

    void set_lock_type(LockType ltype, size_t maxMem) {
        maxMem = align_up(maxMem, align_size*64);
        if (LockMutex == ltype) {
            if (!free_list_lock_free.empty()) {
                THROW_STD(logic_error, "when set lock mutex, mempool must not be lock free");
            }
            if (free_list_mutex.empty()) {
                free_list_mutex.reserve(free_list_len);
                for (size_t i = 0; i < free_list_len; ++i)
                    free_list_mutex.unchecked_emplace_back();
            } else {
                assert(free_list_mutex.size() == free_list_len);
            }
        } else if (LockFree == ltype) {
            if (mem::n > 0) {
                THROW_STD(logic_error, "when set lock free, mempool must be empty");
            }
            if (free_list_arr) {
                free(free_list_arr);
                free_list_arr = NULL;
            }
            if (free_list_lock_free.empty()) {
                free_list_lock_free.resize(free_list_len);
            } else {
                assert(free_list_mutex.size() == free_list_len);
            }
        } else {
            assert(LockNone == ltype);
            free_list_mutex.clear();
            free_list_lock_free.clear();
        }
        mem::reserve(maxMem);
    }

    bool is_allow_concurrent() const {
        return (free_list_mutex.size() | free_list_lock_free.size()) != 0;
    }

    bool is_lock_free() const {
        return free_list_lock_free.size() != 0;
    }

    bool is_lock_mutex() const {
        return free_list_mutex.size() != 0;
    }

    // keep free_list_arr
    void clear() {
        huge_list.size = 0;
        for(auto& next : huge_list.next) next = list_tail;
        fragment_size = 0;
        huge_size_sum = 0;
        huge_node_cnt = 0;
        free_list_lock_free.fill(LockFreeHead());
        std::uninitialized_fill_n(free_list_arr, free_list_len, head_t());
        mem::clear();
    }

    void erase_all() {
        huge_list.size = 0;
        for(auto& next : huge_list.next) next = list_tail;
        fragment_size = 0;
        huge_size_sum = 0;
        huge_node_cnt = 0;
        free_list_lock_free.fill(LockFreeHead());
        std::uninitialized_fill_n(free_list_arr, free_list_len, head_t());
        mem::erase_all();
    }

    void resize_no_init(size_t newsize) {
        assert(newsize % align_size == 0);
        assert(newsize >= mem::size());
        mem::resize_no_init(newsize);
    }

    void shrink_to_fit() {
        mem::shrink_to_fit();
    }

    template<class U> const U& at(size_t pos) const {
        assert(pos < n);
    //  assert(pos + sizeof(U) < n);
        return *(U*)(p + pos);
    }
    template<class U> U& at(size_t pos) {
        assert(pos < n);
    //  assert(pos + sizeof(U) < n);
        return *(U*)(p + pos);
    }

    // param request must be aligned by align_size
    size_t alloc(size_t request) {
        assert(request > 0);
        request = align_up(request, AlignSize);
        if (AlignSize < sizeof(link_t)) // const expression
            request = std::max(sizeof(link_t), request);
        auto freelist = free_list_arr;
        auto database = mem::p;
        size_t res = list_tail;
        if (request <= free_list_len * align_size) {
            size_t idx = request / align_size - 1;
            if (LockFreeHead* lockfree = free_list_lock_free.data()) {
                assert(free_list_lock_free.size() == free_list_len);
                auto& head = as_atomic(lockfree[idx].head);
                link_size_t next = head.load(std::memory_order_relaxed);
                while (list_tail != next) {
                    if (head.compare_exchange_weak(next,
                        *(link_size_t*)(database + AlignSize*size_t(next)),
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                    {
                        as_atomic(lockfree[idx].cnt).fetch_sub(1, std::memory_order_relaxed);
                        as_atomic(fragment_size).fetch_sub(request, std::memory_order_relaxed);
                        return size_t(next) * AlignSize;
                    }
                }
                goto LockFreeIncN;
            }
            else if (std::mutex* mtx = free_list_mutex.data()) {
                assert(free_list_mutex.size() == free_list_len);
                if (list_tail != freelist[idx].head) {
                    mtx[idx].lock();
                    if (list_tail != freelist[idx].head) {
                        assert(fragment_size >= request);
                        assert(freelist[idx].cnt > 0);
                        res = size_t(freelist[idx].head) << offset_shift;
                        assert(res + request <= this->n);
                        freelist[idx].head = (link_t&)(database[res]);
                        freelist[idx].cnt--;
                        mtx[idx].unlock();
                        as_atomic(fragment_size).fetch_sub(request, std::memory_order_relaxed);
                    } else {
                        mtx[idx].unlock();
                    }
                }
            }
            else {
                if (list_tail != freelist[idx].head) {
                    assert(fragment_size >= request);
                    res = size_t(freelist[idx].head) << offset_shift;
                    assert(res + request <= this->n);
                    freelist[idx].head = (link_t&)(database[res]);
                    freelist[idx].cnt--;
                    fragment_size -= request;
                }
            }
        }
        else { // find in freelist, use first match
            assert(request >= sizeof(huge_link_t));
            huge_link_t* update[skip_list_level_max];
            huge_link_t* n1 = &huge_list;
            huge_link_t* n2 = nullptr;
            if (free_list_mutex.size() | free_list_lock_free.size()) {
                assert((free_list_mutex.size() | free_list_lock_free.size()) == free_list_len);
                huge_mutex.lock();
            }
            size_t k = huge_list.size;
            while (k-- > 0) {
                while (n1->next[k] != list_tail && (n2 = &at<huge_link_t>(size_t(n1->next[k]) << offset_shift))->size < request)
                    n1 = n2;
                update[k] = n1;
            }
            if (n2 != nullptr && n2->size >= request) {
                assert((byte*)n2 >= p);
                size_t remain = n2->size - request;
                res = size_t((byte*)n2 - p);
                size_t res_shift = res >> offset_shift;
                for (k = 0; k < huge_list.size; ++k)
                    if ((n1 = update[k])->next[k] == res_shift)
                        n1->next[k] = n2->next[k];
                while (huge_list.next[huge_list.size - 1] == list_tail && --huge_list.size > 0)
                    ;
                if (remain)
                    sfree(res + request, remain);
                as_atomic(fragment_size).fetch_sub(request, std::memory_order_relaxed);
                huge_size_sum -= request;
                huge_node_cnt--; // do not need atomic
            }
            if (free_list_mutex.size() | free_list_lock_free.size()) {
                huge_mutex.unlock();
            }
        }
        if (list_tail == res) {
            if (free_list_lock_free.size()) {
                assert(free_list_lock_free.size() == free_list_len);
            LockFreeIncN:
                size_t pos = as_atomic(n).load(std::memory_order_relaxed);
                size_t cap = mem::c;
                size_t End;
                while ((End = pos + request) <= cap) {
                    if (as_atomic(mem::n).compare_exchange_weak(pos, End,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                        return pos;
                    }
                }
                return size_t(-1); // fail
            }
            else if (free_list_mutex.empty()) {
                ensure_capacity(n + request);
                res = n;
                n += request;
            }
            else {
                update_n_mutex.lock();
                if (n + request <= mem::c) {
                    res = n;
                    n += request;
                } else {
                    // when allow concurrent, this function may fail
                    // with return value = -1
                    res = size_t(-1);
                }
                update_n_mutex.unlock();
            }
        }
        return res;
    }

    size_t alloc3(size_t pos, size_t oldlen, size_t newlen) {
        assert(newlen > 0); newlen = align_up(newlen, AlignSize);
        assert(oldlen > 0); oldlen = align_up(oldlen, AlignSize);
        if (AlignSize < sizeof(link_t)) { // const expression
            oldlen = std::max(sizeof(link_t), oldlen);
            newlen = std::max(sizeof(link_t), newlen);
        }
        assert(pos < n);
        assert(pos + oldlen <= n);
        if (!free_list_mutex.empty()) {
            update_n_mutex.lock();
            assert(pos < n);
            assert(pos + oldlen <= n);
            if (pos + oldlen == n && pos + newlen <= mem::c) {
                n = pos + newlen;
                update_n_mutex.unlock();
                return pos;
            }
            update_n_mutex.unlock();
        } else {
            assert(pos < n);
            assert(pos + oldlen <= n);
            if (pos + oldlen == n) {
                ensure_capacity(pos + newlen);
                n = pos + newlen;
                return pos;
            }
        }
        if (newlen < oldlen) {
            assert(oldlen - newlen >= sizeof(link_t));
            assert(oldlen - newlen >= align_size);
            sfree(pos + newlen, oldlen - newlen);
            return pos;
        }
        else if (newlen == oldlen) {
            // do nothing
            return pos;
        }
        else {
            size_t newpos = alloc(newlen);
            memcpy(p + newpos, p + pos, std::min(oldlen, newlen));
            sfree(pos, oldlen);
            return newpos;
        }
    }

    void sfree(size_t pos, size_t len) {
        assert(len > 0);
        assert(pos < n);
        assert(pos % AlignSize == 0);
        len = align_up(len, AlignSize);
        if (AlignSize < sizeof(link_t)) // const expression
            len = std::max(sizeof(link_t), len);
        assert(pos + len <= n);
        if (LockFreeHead* lockfree = free_list_lock_free.data()) {
            assert(free_list_lock_free.size() == free_list_len);
            size_t End = pos + len;
            if (as_atomic(mem::n).compare_exchange_weak(End, pos,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
            {
                return;
            }
            if (len <= free_list_len * align_size) {
                auto database = mem::p;
                size_t idx = len / align_size - 1;
                auto& head = as_atomic(lockfree[idx].head);
                *(link_size_t*)(database + pos) = head.load(std::memory_order_relaxed);
                link_size_t posVal = link_size_t(pos / AlignSize);
                while (!head.compare_exchange_weak(
                        *(link_size_t*)(database + pos), posVal,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                {}
                as_atomic(lockfree[idx].cnt).fetch_sub(1, std::memory_order_relaxed);
                as_atomic(fragment_size).fetch_sub(len, std::memory_order_relaxed);
                return;
            }
            goto HugeFree;
        }
        if (!free_list_mutex.empty()) {
            if (terark_unlikely(pos + len == n)) {
                update_n_mutex.lock();
                assert(pos < n);
                assert(pos + len <= n);
                if (pos + len == n) {
                    n = pos;
                    update_n_mutex.unlock();
                    return;
                }
                update_n_mutex.unlock();
            }
        }
        else {
            assert(pos < n);
            assert(pos + len <= n);
            if (pos + len == n) {
                n = pos;
                return;
            }
        }
        if (len <= free_list_len * align_size) {
            size_t idx = len / align_size - 1;
            std::mutex* mtx = free_list_mutex.data();
            auto freelist = free_list_arr;
            auto database = mem::p;
            if (mtx) {
                assert(free_list_mutex.size() == free_list_len);
                mtx[idx].lock();
                (link_t&)(database[pos]) = freelist[idx].head;
                freelist[idx].head = link_size_t(pos >> offset_shift);
                freelist[idx].cnt++;
                mtx[idx].unlock();
                as_atomic(fragment_size).fetch_add(len, std::memory_order_relaxed);
            }
            else {
                (link_t&)(database[pos]) = freelist[idx].head;
                freelist[idx].head = link_size_t(pos >> offset_shift);
                freelist[idx].cnt++;
                fragment_size += len;
            }
        }
        else {
        HugeFree:
            assert(len >= sizeof(huge_link_t));
            huge_link_t* update[skip_list_level_max];
            huge_link_t* n1 = &huge_list;
            huge_link_t* n2;
            if (free_list_mutex.size() | free_list_lock_free.size()) {
                assert((free_list_mutex.size() | free_list_lock_free.size()) == free_list_len);
                huge_mutex.lock();
            }
            size_t k = huge_list.size;
            while (k-- > 0) {
                while (n1->next[k] != list_tail && (n2 = &at<huge_link_t>(size_t(n1->next[k]) << offset_shift))->size < len)
                    n1 = n2;
                update[k] = n1;
            }
            k = random_level();
            if (k >= huge_list.size) {
                k = huge_list.size++;
                update[k] = &huge_list;
            };
            n2 = &at<huge_link_t>(pos);
            size_t pos_shift = pos >> offset_shift;
            do {
                n1 = update[k];
                n2->next[k] = n1->next[k];
                n1->next[k] = pos_shift;
            } while(k-- > 0);
            n2->size = len;
            huge_size_sum += len;
            huge_node_cnt++; // do not need atomic
            if (free_list_mutex.size() | free_list_lock_free.size()) {
                huge_mutex.unlock();
                as_atomic(fragment_size).fetch_add(len, std::memory_order_relaxed);
            } else {
                fragment_size += len;
            }
        }
    }

    size_t free_size() const { return fragment_size; }

    void swap(MemPool_ThisType& y) {
        mem::swap(y);
        std::swap(free_list_arr, y.free_list_arr);
        std::swap(free_list_len, y.free_list_len);
        std::swap(fragment_size, y.fragment_size);
        std::swap(huge_size_sum, y.huge_size_sum);
        std::swap(huge_node_cnt, y.huge_node_cnt);
        std::swap(huge_list, y.huge_list);
        // don't swap huge_mutex
        std::swap(free_list_mutex, y.free_list_mutex);
        std::swap(free_list_lock_free, y.free_list_lock_free);
    }

    template<class DataIO>
    friend void DataIO_loadObject(DataIO& dio, MemPool_ThisType& self) {
        typename DataIO::my_var_size_t var;
        self.clear();
        if (self.free_list_arr)
            ::free(self.free_list_arr);
        self.free_list_arr = NULL;
        self.free_list_len = 0;
        self.fragment_size = 0;
        self.huge_size_sum = 0;
        self.huge_node_cnt = 0;
        self.huge_list.size = 0;
        for (auto& next : self.huge_list.next) next = list_tail;
        dio >> var; self.huge_list.size = var.t;
        for (auto& next : self.huge_list.next) {
            dio >> var;
            next = var.t;
        }
        dio >> var;  self.fragment_size = var.t;
        dio >> var;  self.huge_size_sum = var.t;
        dio >> var;  self.huge_node_cnt = var.t;
        dio >> var;  self.free_list_len = var.t;
        self.free_list_arr = (head_t*)malloc(sizeof(head_t) * self.free_list_len);
        if (NULL == self.free_list_arr) {
            self.free_list_arr = NULL;
            self.free_list_len = 0;
            self.fragment_size = 0;
            self.huge_size_sum = 0;
            self.huge_node_cnt = 0;
            self.huge_list.size = 0;
            for (auto& next : self.huge_list.next) next = list_tail;
            throw std::bad_alloc();
        }
        size_t  fastnum = self.free_list_len;
        head_t* fastbin = self.free_list_arr;
        for (size_t i = 0; i < fastnum; ++i) {
            dio >> var;
            fastbin[i].head = link_size_t(var.t);
            dio >> var;
            fastbin[i].cnt = link_size_t(var.t);
        }
        dio >> static_cast<mem&>(self);
    }

    template<class DataIO>
    friend void DataIO_saveObject(DataIO& dio, const MemPool_ThisType& self) {
        const size_t  fastnum = self.free_list_len;
        const head_t* fastbin = self.free_list_arr;
        typename DataIO::my_var_size_t var;
        dio << typename DataIO::my_var_size_t(self.huge_list.size);
        for (auto& next : self.huge_list.next)
            dio << typename DataIO::my_var_size_t(next);
        dio << typename DataIO::my_var_size_t(self.fragment_size);
        dio << typename DataIO::my_var_size_t(self.huge_size_sum);
        dio << typename DataIO::my_var_size_t(self.huge_node_cnt);
        dio << typename DataIO::my_var_size_t(fastnum);
        for (size_t i = 0; i < fastnum; ++i) {
            dio << typename DataIO::my_var_size_t(fastbin[i].head);
            dio << typename DataIO::my_var_size_t(fastbin[i].cnt);
        }
        dio << static_cast<const mem&>(self);
    }
};
#undef MemPool_ThisType

} // namespace terark

