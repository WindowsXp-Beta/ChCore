// clang-format off
/*
 * Copyright (c) 2022 Institute of Parallel And Distributed Systems (IPADS)
 * ChCore-Lab is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
// clang-format on

#include "lab5_stdio.h"

extern struct ipc_struct *tmpfs_ipc_struct;

/* You could add new functions or include headers here.*/
/* LAB 5 TODO BEGIN */

#include <chcore/console.h>

int alloc_fd()
{
        // 0: stdin 1: stdout 2: stderr
        static int fd = 3;
        return fd++;
}
/* LAB 5 TODO END */

int open_file(const char *filename, FILE *f)
{
        struct ipc_msg *ipc_msg =
                ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request), 0);
        chcore_assert(ipc_msg);
        struct fs_request *fr = (struct fs_request *)ipc_get_msg_data(ipc_msg);
        fr->req = FS_REQ_OPEN;
        strcpy(fr->open.pathname, filename);
        fr->open.flags = f->mode;
        fr->open.new_fd = f->fd;
        int ret = ipc_call(tmpfs_ipc_struct, ipc_msg);
        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
        return ret;
}

FILE *fopen(const char *filename, const char *mode)
{
        /* LAB 5 TODO BEGIN */
        FILE *fp = malloc(sizeof(FILE));
        int fd = alloc_fd();
        fp->fd = fd;
        // FIXME(WindowsXp): complete the logic of getting file open mode
        fp->mode = O_RDONLY;
        if (mode[0] == 'w') {
                fp->mode = O_WRONLY;
        }
        if (mode[1] != '\0') {
                fp->mode = O_RDWR;
        }
        int ret = open_file(filename, fp);
        if (ret < 0) {
                if (ret == -2) {
                        printf("create file %s\n", filename);
                        struct ipc_msg *ipc_msg_create = ipc_create_msg(
                                tmpfs_ipc_struct, sizeof(struct fs_request), 0);
                        chcore_assert(ipc_msg_create);
                        struct fs_request *fr_create =
                                (struct fs_request *)ipc_get_msg_data(
                                        ipc_msg_create);
                        fr_create->req = FS_REQ_CREAT;
                        strcpy(fr_create->creat.pathname, filename);
                        ret = ipc_call(tmpfs_ipc_struct, ipc_msg_create);
                        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg_create);
                        if (ret < 0)
                                chcore_bug("create file failed!");
                        // fr->creat.mode: we don't use this field in tmpfs
                        // call open again
                        printf("reopen file %s\n", filename);
                        ret = open_file(filename, fp);
                        if (ret < 0)
                                goto fail;
                } else {
                        goto fail;
                }
        }
        /* LAB 5 TODO END */
        return fp;
fail:
        chcore_bug("open file failed!");
}

size_t fwrite(const void *src, size_t size, size_t nmemb, FILE *f)
{
        /* LAB 5 TODO BEGIN */
        size_t len = size * nmemb;
        struct ipc_msg *ipc_msg = ipc_create_msg(
                tmpfs_ipc_struct, sizeof(struct fs_request) + len, 0);
        chcore_assert(ipc_msg);
        struct fs_request *fr = (struct fs_request *)ipc_get_msg_data(ipc_msg);
        memcpy((void *)fr + sizeof(struct fs_request), src, len);
        fr->req = FS_REQ_WRITE;
        fr->write.count = len;
        fr->write.fd = f->fd;
        int ret = ipc_call(tmpfs_ipc_struct, ipc_msg);
        chcore_bug_on(ret != len);
        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
        /* LAB 5 TODO END */
        return ret;
}

size_t fread(void *destv, size_t size, size_t nmemb, FILE *f)
{
        /* LAB 5 TODO BEGIN */
        size_t len = size * nmemb;
        struct ipc_msg *ipc_msg = ipc_create_msg(
                tmpfs_ipc_struct, sizeof(struct fs_request) + len + 2, 0);
        chcore_assert(ipc_msg);
        struct fs_request *fr = (struct fs_request *)ipc_get_msg_data(ipc_msg);
        fr->req = FS_REQ_READ;
        fr->read.fd = f->fd;
        fr->read.count = len;
        int ret = ipc_call(tmpfs_ipc_struct, ipc_msg);
        memcpy(destv, ipc_get_msg_data(ipc_msg), ret);
        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
        /* LAB 5 TODO END */
        return ret;
}

int fclose(FILE *f)
{
        /* LAB 5 TODO BEGIN */
        struct ipc_msg *ipc_msg =
                ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request), 0);
        chcore_assert(ipc_msg);
        struct fs_request *fr = (struct fs_request *)ipc_get_msg_data(ipc_msg);
        fr->req = FS_REQ_CLOSE;
        fr->close.fd = f->fd;
        int ret = ipc_call(tmpfs_ipc_struct, ipc_msg);
        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
        if (ret < 0) {
                chcore_bug("close file error");
        }
        free(f);
        /* LAB 5 TODO END */
        return 0;
}

/* Need to support %s and %d. */
int fscanf(FILE *f, const char *fmt, ...)
{
        /* LAB 5 TODO BEGIN */
        va_list va;
        va_start(va, fmt);
        char buf[BUF_SIZE] = {'\0'};
        int fmt_p = 0, buf_p = 0, fmt_size = strlen(fmt);
        int file_size = fread(buf, sizeof(char), BUF_SIZE, f);
        while (buf_p < file_size && fmt_p < fmt_size) {
                if (fmt[fmt_p] == '%') {
                        fmt_p++;
                        switch (fmt[fmt_p]) {
                        case ('s'): {
                                char *bind_data = va_arg(va, char *);
                                int string_start = buf_p;
                                while (buf[buf_p] != ' ' && buf_p < file_size) {
                                        buf_p++;
                                }
                                int len = buf_p - string_start;
                                chcore_assert(len > 0);
                                memcpy(bind_data,
                                       (char *)buf + string_start,
                                       len);
                                bind_data[len] = '\0';
                                break;
                        }
                        case ('d'): {
                                int *bind_data = va_arg(va, int *);
                                int num = 0;
                                while (buf[buf_p] >= '0' && buf[buf_p] <= '9'
                                       && buf_p < file_size) {
                                        num = num * 10 + buf[buf_p] - '0';
                                        buf_p++;
                                }
                                *bind_data = num;
                                break;
                        }
                        default: {
                                chcore_bug("unsupported data type");
                        }
                        }
                        fmt_p++;
                } else {
                        chcore_assert(buf[buf_p] == fmt[fmt_p]);
                        fmt_p++;
                        buf_p++;
                }
        }
        /* LAB 5 TODO END */
        return 0;
}

/* Need to support %s and %d. */
int fprintf(FILE *f, const char *fmt, ...)
{
        /* LAB 5 TODO BEGIN */
        char buf[BUF_SIZE];
        char *p = buf;
        va_list va;
        va_start(va, fmt);
        int ret = simple_vsprintf(&p, fmt, va);
        va_end(va);
        if (ret < 0) {
                chcore_bug("fprintf failed");
        }
        ret = fwrite(buf, sizeof(char), ret, f);
        /* LAB 5 TODO END */
        return ret;
}
