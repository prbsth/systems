#include "io61.hh"
#include <climits>
#include <cerrno>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

// defining macro for the size of the regions that the file is divided into 
#define REGION_SIZE  64

// io61.cc
//    YOUR CODE HERE!
 int io61_flush_locked(io61_file* f);

struct reg_lock { //struct for region
        unsigned locked = 0;
        std::thread::id owner;
    };
// io61_file
//    Data structure for io61 file wrappers.
struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // O_RDONLY, O_WRONLY, or O_RDWR
    bool seekable;   // is this file seekable?

    // Single-slot cache
    static constexpr off_t cbufsz = 8192;
    unsigned char cbuf[cbufsz];
    off_t tag;       // offset of first character in `cbuf`
    off_t pos_tag;   // next offset to read or write (non-positioned mode)
    off_t end_tag;   // offset one past last valid character in `cbuf`

    // Positioned mode
    bool dirty = false;       // has cache been written?
    bool positioned = false;  // is cache in positioned mode?

    //mmap
    void* map_addr; //address of map if worked, MAP_FAILED otherwise. 

    //synchronization
    std::recursive_mutex rmutex;
    std::mutex mutex;
    std::condition_variable_any cv;
    reg_lock *reg; // dynamic array for region lcoks
    size_t num_regions; // stores the number of regions. I've made it such that 
    //the file gets divided into regions of 64 bytes so the number of total region hence depends on the file size
    struct stat st;
};

// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file, O_WRONLY for a write-only file,
//    or O_RDWR for a read/write file.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    assert((mode & O_APPEND) == 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode & O_ACCMODE;
    off_t off = lseek(fd, 0, SEEK_CUR);
    if (off != -1) {
        f->seekable = true;
        f->tag = f->pos_tag = f->end_tag = off;
    } else {
        f->seekable = false;
        f->tag = f->pos_tag = f->end_tag = 0;
    }
    f->dirty = f->positioned = false;
    //SYNCHRONIZATION
    fstat(fd, &f->st);
    f->num_regions = ((f->st.st_size) + (REGION_SIZE-1)) /REGION_SIZE; //this gives us the rounded up value of division 
    //(by adding 63, I ensure that any remainder is taken care of, so we get the correct value using integer division)
    // allocating
    f->reg = new reg_lock[f->num_regions];
    f->map_addr = mmap(NULL, f->st.st_size, PROT_READ, MAP_PRIVATE, f->fd, 0);
    return f;
}

//helpers
//I check here if the given region has overlap with any locks, use REGIONSIZE to determine arithmetic (macro defined at top)
bool may_overlap_with_other_lock(io61_file* f, off_t start, off_t len) {
    size_t rstart = start /REGION_SIZE, rend = (start + len - 1) /REGION_SIZE;
    for (size_t ri = rstart; ri <= rend; ++ri) {
        if (f->reg[ri].locked > 0 && f->reg[ri].owner != std::this_thread::get_id()) {
            return true;
        }
    }
    return false;
}

// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    delete[] f->reg;
    if (f->map_addr != MAP_FAILED){
        munmap(f->map_addr, io61_filesize(f)); //closing map if exists
    }
    delete f;
    return r;
}


// NORMAL READING AND WRITING FUNCTIONS

// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

static int io61_fill(io61_file* f);

int io61_readc(io61_file* f) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    assert(!f->positioned);
    if (f->pos_tag == f->end_tag) {
        io61_fill(f);
        if (f->pos_tag == f->end_tag) {
            return -1;
        }
    }
    unsigned char ch = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    return ch;
}


// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    assert(!f->positioned);
    size_t nread = 0;
    if (f->map_addr!=MAP_FAILED){ //same as pset4
        ssize_t bytes_to_copy = std::min(sz, (size_t)(f->st.st_size - f->pos_tag));
        memcpy(buf + nread, (unsigned char*)f->map_addr + f->pos_tag, bytes_to_copy); //same as pset4
        f->pos_tag += bytes_to_copy;
        nread = bytes_to_copy;
    }
    while (nread != sz) {
        if (f->pos_tag == f->end_tag) {
            int r = io61_fill(f);
            if (r == -1 && nread == 0) {
                return -1;
            } else if (f->pos_tag == f->end_tag) {
                break;
            }
        }
        size_t nleft = f->end_tag - f->pos_tag;
        size_t ncopy = std::min(sz - nread, nleft);
        memcpy(&buf[nread], &f->cbuf[f->pos_tag - f->tag], ncopy);
        nread += ncopy;
        f->pos_tag += ncopy;
    }
    return nread;
}


// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    assert(!f->positioned);
    if (f->pos_tag == f->tag + f->cbufsz) {
        int r = io61_flush_locked(f);
        if (r == -1) {
            return -1;
        }
    }
    f->cbuf[f->pos_tag - f->tag] = c;
    ++f->pos_tag;
    ++f->end_tag;
    f->dirty = true;
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    assert(!f->positioned);
    size_t nwritten = 0;
    while (nwritten != sz) {
        if (f->end_tag == f->tag + f->cbufsz) {
            int r = io61_flush_locked(f);
            if (r == -1 && nwritten == 0) {
                return -1;
            } else if (r == -1) {
                break;
            }
        }
        size_t nleft = f->tag + f->cbufsz - f->pos_tag;
        size_t ncopy = std::min(sz - nwritten, nleft);
        memcpy(&f->cbuf[f->pos_tag - f->tag], &buf[nwritten], ncopy);
        f->pos_tag += ncopy;
        f->end_tag += ncopy;
        f->dirty = true;
        nwritten += ncopy;
    }
    return nwritten;
}


// io61_flush(f)
//    If `f` was opened for writes, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only and is seekable, `io61_flush(f)` drops any
//    data cached for reading and seeks to the logical file position.

static int io61_flush_dirty(io61_file* f);
static int io61_flush_dirty_positioned(io61_file* f);
static int io61_flush_clean(io61_file* f);

int io61_flush(io61_file* f) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    if (f->dirty && f->positioned) {
        return io61_flush_dirty_positioned(f);
    } else if (f->dirty) {
        return io61_flush_dirty(f);
    } else {
        return io61_flush_clean(f);
    }
}

int io61_flush_locked(io61_file* f) {
    if (f->dirty && f->positioned) {
        return io61_flush_dirty_positioned(f);
    } else if (f->dirty) {
        return io61_flush_dirty(f);
    } else {
        return io61_flush_clean(f);
    }
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    if (f->map_addr != MAP_FAILED){ //if map
        off_t filesize = io61_filesize(f);
        if (off > filesize){
            return -1;
        }
        else {
            f->pos_tag = off;
            return 0;
        }
    }
    int r = io61_flush_locked(f);
    if (r == -1) {
        return -1;
    }
    off_t roff = lseek(f->fd, off, SEEK_SET);
    if (roff == -1) {
        return -1;
    }
    f->tag = f->pos_tag = f->end_tag = off;
    f->positioned = false;
    return 0;
}


// Helper functions

// io61_fill(f)
//    Fill the cache by reading from the file. Returns 0 on success,
//    -1 on error. Used only for non-positioned files.

static int io61_fill(io61_file* f) {
    assert(f->tag == f->end_tag && f->pos_tag == f->end_tag);
    ssize_t nr;
    while (true) {
        nr = read(f->fd, f->cbuf, f->cbufsz);
        if (nr >= 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
    f->end_tag += nr;
    return 0;
}


// io61_flush_*(f)
//    Helper functions for io61_flush.

static int io61_flush_dirty(io61_file* f) {
    // Called when `f`’s cache is dirty and not positioned.
    // Uses `write`; assumes that the initial file position equals `f->tag`.
    off_t flush_tag = f->tag;
    while (flush_tag != f->end_tag) {
        ssize_t nw = write(f->fd, &f->cbuf[flush_tag - f->tag],
                           f->end_tag - flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    f->tag = f->pos_tag = f->end_tag;
    return 0;
}

static int io61_flush_dirty_positioned(io61_file* f) {
    // Called when `f`’s cache is dirty and positioned.
    // Uses `pwrite`; does not change file position.
    off_t flush_tag = f->tag;
    while (flush_tag != f->end_tag) {
        ssize_t nw = pwrite(f->fd, &f->cbuf[flush_tag - f->tag],
                            f->end_tag - flush_tag, flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    return 0;
}

static int io61_flush_clean(io61_file* f) {
    // Called when `f`’s cache is clean.
    if (!f->positioned && f->seekable) {
        if (lseek(f->fd, f->pos_tag, SEEK_SET) == -1) {
            return -1;
        }
        f->tag = f->end_tag = f->pos_tag;
    }
    return 0;
}



// POSITIONED I/O FUNCTIONS

// io61_pread(f, buf, sz, off)
//    Read up to `sz` bytes from `f` into `buf`, starting at offset `off`.
//    Returns the number of characters read or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

static int io61_pfill(io61_file* f, off_t off);

ssize_t io61_pread(io61_file* f, unsigned char* buf, size_t sz,
                   off_t off) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(buf, &f->cbuf[off - f->tag], ncopy);
    return ncopy;
}


// io61_pwrite(f, buf, sz, off)
//    Write up to `sz` bytes from `buf` into `f`, starting at offset `off`.
//    Returns the number of characters written or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

ssize_t io61_pwrite(io61_file* f, const unsigned char* buf, size_t sz,
                    off_t off) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);                 
    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(&f->cbuf[off - f->tag], buf, ncopy);
    f->dirty = true;
    return ncopy;
}


// io61_pfill(f, off)
//    Fill the single-slot cache with data including offset `off`.
//    The handout code rounds `off` down to a multiple of 8192.

static int io61_pfill(io61_file* f, off_t off) {
    std::lock_guard<std::recursive_mutex> lock(f->rmutex);
    assert(f->mode == O_RDWR);
    if (f->dirty && io61_flush_locked(f) == -1) {
        return -1;
    }

    off = off - (off % 8192);
    ssize_t nr = pread(f->fd, f->cbuf, f->cbufsz, off);
    if (nr == -1) {
        return -1;
    }
    f->tag = off;
    f->end_tag = off + nr;
    f->positioned = true;
    return 0;
}



// FILE LOCKING FUNCTIONS

// io61_try_lock(f, start, len, locktype)
//    Attempts to acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 if it was not. Does not
//    block: if the lock cannot be acquired, it returns -1 right away.

// int io61_try_lock(io61_file* f, off_t start, off_t len, int locktype) {
//     (void) f;
//     assert(start >= 0 && len >= 0);
//     assert(locktype == LOCK_EX || locktype == LOCK_SH);
//     if(f->mutex.try_lock()){
//         return 0;
//     }
//     else{
//         return -1;
//     }
// }
int io61_try_lock(io61_file* f, off_t start, off_t len, int locktype) {
    (void) f;
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) {
        return 0;
    }
    std::unique_lock<std::mutex> guard(f->mutex); //making a guard
    if (may_overlap_with_other_lock(f, start, len)) { //section inspired may_overlap function
        return -1;
    }
    size_t rstart = start /REGION_SIZE, rend = (start + len - 1) /REGION_SIZE; //going to the region start and looping
    for (size_t ri = rstart; ri <= rend; ++ri) {
        ++f->reg[ri].locked;
        f->reg[ri].owner = std::this_thread::get_id();
    }
    return 0;
}


// io61_lock(f, start, len, locktype)
//    Acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 on error. Blocks until
//    the lock can be acquired; the -1 return value is reserved for true
//    error conditions, such as EDEADLK (a deadlock was detected).

int io61_lock(io61_file* f, off_t start, off_t len, int locktype) {
    //the implemetation is same as try_lock above but just with cv.wait if 
    // there is overlap (rather than immediately returning -1)
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) {
        return 0;
    }
    std::unique_lock<std::mutex> guard(f->mutex);
    while (may_overlap_with_other_lock(f, start, len)) {
        f->cv.wait(guard);
    }
    size_t rstart = start /REGION_SIZE, rend = (start + len - 1) /REGION_SIZE;
    for (size_t ri = rstart; ri <= rend; ++ri) {
        ++f->reg[ri].locked;
        f->reg[ri].owner = std::this_thread::get_id();
    }
    return 0;
}


// io61_unlock(f, start, len)
//    Release the lock on offsets `[start,len)` in file `f`.
//    Returns 0 on success and -1 on error.

int io61_unlock(io61_file* f, off_t start, off_t len) {
    (void) f;
    if (len == 0) {
        return 0;
    }
    std::unique_lock<std::mutex> guard(f->mutex); //guard to lock during
    size_t rstart = start /REGION_SIZE, rend = (start + len - 1) /REGION_SIZE;
    for (size_t ri = rstart; ri <= rend; ++ri) {
        --f->reg[ri].locked; //release
    }
    f->cv.notify_all();
    return 0;
}



// HELPER FUNCTIONS
// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) { //okay but like why does this function just not work, 
//why do I have to do the stat thing, using this function for filesize never works for my code
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}
