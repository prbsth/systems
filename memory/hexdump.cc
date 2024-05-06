ssize_t io61_write(io61_fcache* f, const unsigned char* buf, size_t sz) {
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);  // Check invariants
    assert(f->pos_tag == f->end_tag); // Write cache invariant

    size_t pos = 0; // Position in the buffer `buf`
    while (pos < sz) {
        size_t remaining_buffer_space = f->bufsize - (f->pos_tag - f->tag);
        if (remaining_buffer_space == 0) {
            io61_flush(f);  // Flush buffer to disk when full
            remaining_buffer_space = f->bufsize; // After flush, whole buffer is available
        }

        // Calculate the amount of data to write in this iteration
        size_t bytes_to_write = std::min(remaining_buffer_space, sz - pos);

        // Use `memcpy` to copy data from `buf` to cache buffer
        memcpy(f->cbuf + (f->pos_tag - f->tag), buf + pos, bytes_to_write);

        f->pos_tag += bytes_to_write; // Update cache's current position
        if (f->pos_tag > f->end_tag) {
            f->end_tag = f->pos_tag; // Update end tag only when writing beyond current data
        }
        pos += bytes_to_write; // Move forward in the input buffer
    }

    return pos; // Return the number of bytes written
}





void io61_flush(io61_fcache* f) {
    // Actual implementation to flush buffer to the file.
    // Should handle partial writes and errors.
    // Resets buffer positions after successful flush.
}

ssize_t io61_write(io61_fcache* f, const unsigned char* buf, size_t sz) {
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    size_t pos = 0;
    bool buffer_modified = false;
    while (pos < sz) {
        // Determine the space left in the buffer and the amount to write.
        size_t space_in_buffer = f->bufsize - (f->pos_tag - f->tag);
        size_t bytes_to_write = std::min(space_in_buffer, sz - pos);

        // If the buffer is full and the next write is not an overwrite, flush it.
        if (space_in_buffer == 0 && f->pos_tag != f->tag) {
            io61_flush(f);  // Flush the buffer to file.
            f->tag = f->pos_tag;  // Move tag to the current file position.
            buffer_modified = false;  // Reset the modified flag.
            space_in_buffer = f->bufsize;  // Reset space in buffer.
        }

        // Perform the write into the buffer.
        memcpy(f->cbuf + (f->pos_tag - f->tag), buf + pos, bytes_to_write);
        pos += bytes_to_write;
        f->pos_tag += bytes_to_write;
        f->end_tag = std::max(f->end_tag, f->pos_tag);  // Extend the end tag if needed.
        buffer_modified = true;
    }

    // or leave it to be handled by a file close operation or explicit flush.
    if (buffer_modified) {
        io61_flush(f);  // Flush the buffer if it was modified.
    }

    return pos;  // Return the number of bytes written.
}
