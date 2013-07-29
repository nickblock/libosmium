#ifndef OSMIUM_INDEX_MAP_MMAP_FILE_HPP
#define OSMIUM_INDEX_MAP_MMAP_FILE_HPP

/*

This file is part of Osmium (http://osmcode.org/osmium).

Copyright 2013 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>

#include <osmium/index/map.hpp>

namespace osmium {

    namespace index {

        namespace map {

            /**
            * MmapFile stores data in files using the mmap() system call.
            * It will grow automatically.
            *
            * If you have enough memory it is preferred to use the in-memory
            * version MmapAnon. If you don't have enough memory or want the
            * data to persist, use this version. Note that in any case you need
            * substantial amounts of memory for this to work efficiently.
            */
            template <typename TValue>
            class MmapFile : public osmium::index::map::Base<TValue> {

                uint64_t m_size;

                TValue* m_items;

                int m_fd;

                /// Get file size in bytes.
                uint64_t get_file_size() const {
                    struct stat s;
                    if (fstat(m_fd, &s) < 0) {
                        throw std::bad_alloc();
                    }
                    return s.st_size;
                }

            public:

                static const uint64_t size_increment = 10 * 1024 * 1024;

                /**
                * Create mapping backed by file. If filename is empty, a temporary
                * file will be created.
                *
                * @param filename The filename (including the path) for the storage.
                * @param remove Should the file be removed after use?
                * @exception std::bad_alloc Thrown when there is not enough memory or some other problem.
                */
                MmapFile(const std::string& filename="", bool remove=true) :
                    Base<TValue>(),
                    m_size(1) {
                    if (filename == "") {
                        FILE* file = tmpfile();
                        if (!file) {
                            throw std::bad_alloc();
                        }
                        m_fd = fileno(file);
                    } else {
                        m_fd = open(filename.c_str(), O_RDWR | O_CREAT, 0600);
                    }

                    if (m_fd < 0) {
                        throw std::bad_alloc();
                    }

                    // now that the file is open we can immediately remove it
                    // (temporary files are always removed)
                    if (remove && filename != "") {
                        if (unlink(filename.c_str()) < 0) {
                            // XXX what to do here?
                        }
                    }

                    // make sure the file is at least as large as the initial size
                    if (get_file_size() < sizeof(TValue) * m_size) {
                        if (ftruncate(m_fd, sizeof(TValue) * m_size) < 0) {
                            throw std::bad_alloc();
                        }
                    }

                    m_items = static_cast<TValue*>(mmap(NULL, sizeof(TValue) * m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
                    if (m_items == MAP_FAILED) {
                        throw std::bad_alloc();
                    }
                }

                ~MmapFile() {
                    clear();
                }

                void set(const uint64_t id, const TValue value) {
                    if (id >= m_size) {
                        uint64_t new_size = id + size_increment;

                        // if the file backing this mmap is smaller than needed, increase its size
                        if (get_file_size() < sizeof(TValue) * new_size) {
                            if (ftruncate(m_fd, sizeof(TValue) * new_size) < 0) {
                                throw std::bad_alloc();
                            }
                        }

                        if (munmap(m_items, sizeof(TValue) * m_size) < 0) {
                            throw std::bad_alloc();
                        }
                        m_items = static_cast<TValue*>(mmap(NULL, sizeof(TValue) * new_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
                        if (m_items == MAP_FAILED) {
                            throw std::bad_alloc();
                        }
                        m_size = new_size;
                    }
                    m_items[id] = value;
                }

                const TValue operator[](const uint64_t id) const {
                    return m_items[id];
                }

                uint64_t size() const {
                    return m_size;
                }

                uint64_t used_memory() const {
                    return m_size * sizeof(TValue);
                }

                void clear() {
                    munmap(m_items, sizeof(TValue) * m_size);
                }

            }; // class MmapFile

        } // namespace map

    } // namespace index

} // namespace osmium

#endif // OSMIUM_INDEX_MAP_MMAP_FILE_HPP
