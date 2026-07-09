/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "storage/disk_manager.h"

#include <algorithm>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "defs.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

DiskManager::DiskManager() { memset(fd2pageno_, 0, MAX_FD * (sizeof(std::atomic<page_id_t>) / sizeof(char))); }

void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    if (lseek(fd, static_cast<off_t>(page_no) * PAGE_SIZE, SEEK_SET) == -1) {
        throw UnixError();
    }
    int bytes_written = 0;
    while (bytes_written < num_bytes) {
        ssize_t ret = write(fd, offset + bytes_written, num_bytes - bytes_written);
        if (ret < 0) {
            throw UnixError();
        }
        if (ret == 0) {
            throw InternalError("DiskManager::write_page Error");
        }
        bytes_written += static_cast<int>(ret);
    }
}

void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    if (lseek(fd, static_cast<off_t>(page_no) * PAGE_SIZE, SEEK_SET) == -1) {
        throw UnixError();
    }
    int bytes_read = 0;
    while (bytes_read < num_bytes) {
        ssize_t ret = read(fd, offset + bytes_read, num_bytes - bytes_read);
        if (ret < 0) {
            throw UnixError();
        }
        if (ret == 0) {
            throw InternalError("DiskManager::read_page Error");
        }
        bytes_read += static_cast<int>(ret);
    }
}

page_id_t DiskManager::allocate_page(int fd) {
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    std::string cmd = "mkdir " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

bool DiskManager::is_file(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void DiskManager::create_file(const std::string &path) {
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_BINARY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        throw UnixError();
    }
    if (close(fd) < 0) {
        throw UnixError();
    }
}

void DiskManager::destroy_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path) != 0) {
        throw FileNotClosedError(path);
    }
    if (unlink(path.c_str()) < 0) {
        throw UnixError();
    }
}

int DiskManager::open_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path) != 0) {
        throw FileNotClosedError(path);
    }
    int fd = open(path.c_str(), O_RDWR | O_BINARY);
    if (fd < 0) {
        throw UnixError();
    }
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    int file_size = get_file_size(path);
    fd2pageno_[fd] = file_size <= 0 ? 0 : (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    return fd;
}

void DiskManager::close_file(int fd) {
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }
    std::string path = it->second;
    if (close(fd) < 0) {
        throw UnixError();
    }
    fd2path_.erase(it);
    path2fd_.erase(path);
}

int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

int DiskManager::get_file_fd(const std::string &file_name) {
    if (!path2fd_.count(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}

int DiskManager::read_log(char *log_data, int size, int offset) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if (size == 0) {
        return 0;
    }
    lseek(log_fd_, offset, SEEK_SET);
    ssize_t bytes_read = read(log_fd_, log_data, size);
    assert(bytes_read == size);
    return bytes_read;
}

void DiskManager::write_log(char *log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    lseek(log_fd_, 0, SEEK_END);
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) {
        throw UnixError();
    }
}
