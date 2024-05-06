//MEMORY MAPPING BABY

#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
// io61.cc
//    YOUR CODE HERE!


// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // open mode (O_RDONLY or O_WRONLY)
    static constexpr off_t bufsize = 4096; //blocksize
    unsigned char cbuf[bufsize]; //buffer for cache
    off_t tag; //file offset of first byte of cached data
    off_t end_tag; //file offset one past the last byte
    off_t pos_tag; //pos within the cache
    void* map_addr; //address of map if worked, MAP_FAILED otherwise. 
    struct stat st; //filesize
};


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode;
    f->tag = 0;
    f->end_tag = 0;
    f->pos_tag = 0; 
    f->map_addr = mmap(NULL, f->st.st_size, PROT_READ, MAP_PRIVATE, f->fd, 0);
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    if (f->map_addr != MAP_FAILED){
        munmap(f->map_addr, io61_filesize(f)); //closing map if exists
    }   
    int r = close(f->fd);
    delete f;
    return r;
}

ssize_t cache_fill (io61_file* f){
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    f->tag = f->pos_tag = f->end_tag;
    ssize_t n = read(f->fd, f->cbuf, f->bufsize);
    if (n > 0) {
        f->end_tag = f->tag + n;
    }
    if (n <= 0) {
        if (errno == EINTR || errno == EAGAIN){
            cache_fill(f); // trying again
        }
        else {
            return -1;
        }
    }
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);
    return n;
}



// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.
int io61_readc(io61_file* f) {
    if (f->pos_tag == f->end_tag) {
        ssize_t c = cache_fill(f);
        if (c <= 0) {
            return -1;  // Return -1 on end of file or error.
        }
    }
    unsigned char ch = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    return ch;
}


// iteration 2
ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    // Check invariants.
    size_t pos = 0;
    if (f->map_addr == MAP_FAILED){
        assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
        assert(f->end_tag - f->pos_tag <= f->bufsize);
        while (pos < sz) {
            // If the cache is empty, refill it.
            if (f->pos_tag == f->end_tag) {
                ssize_t c = cache_fill(f);
                if (c == 0){
                    break;
                }
                // If the cache is still empty after a fill, we're at EOF or an error occurred.
                if (f->pos_tag == f->end_tag) {
                    break;
                }
            }
            // Determine the number of bytes to read from the cache.
            size_t bytes_to_copy = std::min((long) (f->end_tag - f->pos_tag), (long) (sz - pos));

            // Use memcpy to copy the data into provided buffer
            memcpy(buf + pos, f->cbuf + (f->pos_tag - f->tag), bytes_to_copy);

            // Update the positions in the buffer and cache.
            f->pos_tag += bytes_to_copy;
            pos += bytes_to_copy;
        }
    }
    else { //IF MEMORY MAPPED
        ssize_t bytes_to_copy = std::min(sz, (size_t)(f->st.st_size - f->pos_tag));
        memcpy(buf + pos, (unsigned char*)f->map_addr + f->pos_tag, bytes_to_copy);
        f->pos_tag += bytes_to_copy;
        pos = bytes_to_copy;
        }
    return pos;
}
// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    unsigned char ch = c;
    if (f->end_tag == f->tag + f->bufsize) {
        ssize_t flush = io61_flush(f);
        if (flush < 0) {
            return -1;  // Return -1 on end of file or error.
        }
    }
    memcpy(f->cbuf + (f->pos_tag - f->tag), &ch, 1);
    ++f->pos_tag;
    if (f->pos_tag > f->end_tag) {
            f->end_tag = f->pos_tag; //going beyond
        }
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    // Check invariants.
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    ssize_t pos = 0;
    while (pos < (ssize_t) sz) {
        // If the cache is full, empty it.
        if (f->end_tag == f->tag + f->bufsize) {
            ssize_t fl = io61_flush(f);
            // If the cache is still non-empty after a flush, some error occured or ran out of drive space
            if (fl == -1) {
                return (std::max(((long) pos) - 1, (long) -1)); //checking if anything has already been written, in which case we return that otherwise -1. 
            }
        }
        // Determine the number of bytes to write this time
        size_t bytes_to_write = std::min(sz - pos, (size_t) (f->tag + f->bufsize - f->pos_tag));

        // Use memcpy to copy the data into provided buffer
        memcpy(f->cbuf + (f->pos_tag - f->tag), buf + pos, bytes_to_write);

        // Update the positions in the buffer and cache.
        f->pos_tag += bytes_to_write;
        f->end_tag += bytes_to_write;
        pos += bytes_to_write;
    }
    return pos;
}


// io61_flush(f)
//    If `f` was opened write-only, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. It may also
//    drop any data cached for reading.

int io61_flush(io61_file* f) { //keep retrying until restartable errors go away (check cerrno for error)
    if (f->mode == O_RDONLY){ 
        return 0;
    } 
    assert(f->end_tag - f->pos_tag <= f->bufsize);
    ssize_t towrite = f->pos_tag - f->tag;
    size_t pos = 0;
    while ((ssize_t) pos < towrite) {
        ssize_t n = write(f->fd, f->cbuf + pos, towrite - pos); //writing to memory
        if (n < 0){
            if (errno == EINTR || errno == EAGAIN){
                continue;
            } //if recoverable error
            else
            {
                break;
            }
        }
        pos +=n; //loop next condition
    }
    f->tag = f->pos_tag; //setting tag = postab
    return 0;
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    if(f->map_addr == MAP_FAILED){ //if no mapping
        if (off >= f->tag && off < f-> end_tag){
            f->pos_tag = off;
            return 0;
        }
        if (f->mode == O_RDONLY){
            off_t off_a = off - (off % f->bufsize); //align
            off_t lsresult = lseek(f->fd, off_a, SEEK_SET);
            if(lsresult == -1){
                return -1;
            }
            f->tag = f->end_tag = f->pos_tag = off_a;
            cache_fill(f);
            if(f->pos_tag == f->end_tag){ //error
                return -1;
            }
            f->pos_tag += (off % f->bufsize); //align
            return 0;
        }
        else if (f->mode == O_WRONLY){
            io61_flush(f);
            off_t lsresult = lseek(f->fd, off, SEEK_SET);
            if (lsresult != -1){
                f->tag = f->end_tag = f->pos_tag = off;
            }
            return 0;
        }
        return -1;
    }
    else{ //if mapping then directly set to f->pos_tag since that is the only relevant thing. 
        off_t filesize = io61_filesize(f);
        if (off > filesize){
            return -1;
        }
        else {
            f->pos_tag = off;
        }
    }

}


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

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}

