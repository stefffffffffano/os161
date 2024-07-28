#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <current.h>
#include <lib.h>
#include <kern/fcntl.h>
#include <proc.h>
#include <vnode.h>
#include <vfs.h>
#include <uio.h>
#include <synch.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/seek.h>
#include <stat.h>
#include <endian.h>




/* max num of system wide open files */
#define SYSTEM_OPEN_MAX (10*OPEN_MAX)



struct openfile systemFileTable[SYSTEM_OPEN_MAX];

void openfileIncrRefCount(struct openfile *of) {
    if (of != NULL)
        of->countRef++;
}

int sys_write(int fd, userptr_t buf_ptr, size_t size, int *err) {
    struct iovec iov;
    struct uio ku;
    int result, nwrite;
    struct vnode *vn;
    struct openfile *of;
    void *kbuf;


    if (fd < 0 || fd > OPEN_MAX){
        *err = EBADF;
        return -1;
    } 
    of = curproc->fileTable[fd];
    if (of == NULL){
        *err=EBADF;
        return -1;
    }
    if(of->mode_open != O_WRONLY && of->mode_open!=O_RDWR){
        *err= EBADF;
        return -1;
    }
    vn = of->vn;
    if (vn == NULL){
        *err=EBADF;
        return -1;
    } 

    
    kbuf = kmalloc(size);
    if(kbuf==NULL){
        *err = ENOMEM;
        return -1;
    }
    if(copyin(buf_ptr, kbuf, size)){
        *err=EFAULT; //buf is outside the accessible address space
        kfree(kbuf);
        return -1;
    }
    lock_acquire(of->lock); //writing acquiring the lock to be the only one doing it
    uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_WRITE);
    
    result = VOP_WRITE(vn, &ku);
    if (result) {
        kfree(kbuf);
        *err=result;
        return -1;
    }
    kfree(kbuf);
    of->offset = ku.uio_offset;
    nwrite = size - ku.uio_resid;
    lock_release(of->lock);
    return nwrite;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size, int *err) {
    struct iovec iov;
    struct uio ku;
    int result, nread;
    struct vnode *vn;
    struct openfile *of;
    void *kbuf;

    if (fd < 0 || fd >= OPEN_MAX) {
        *err = EBADF;
        return -1;
    } 
    of = curproc->fileTable[fd];
    if (of == NULL) {
        *err = EBADF;
        return -1;
    }
    if (of->mode_open != O_RDONLY && of->mode_open != O_RDWR) {
        *err = EBADF;
        return -1;
    }
    vn = of->vn;
    if (vn == NULL) {
        *err = EBADF;
        return -1;
    }

   
    kbuf = kmalloc(size);
    if (kbuf == NULL) {
        *err = ENOMEM;
        return -1;
    }
    //Trying to copy the content of the user buffer in a Kernel buffer to make it check its validity
    if (copyin(buf_ptr, kbuf, size)) {
        kfree(kbuf);
        *err = EFAULT;
        return -1;
    }

    lock_acquire(of->lock);

    uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_READ);
    result = VOP_READ(vn, &ku);

    if (result) {
        kfree(kbuf);
        *err = result;
        return -1;
    }
    of->offset = ku.uio_offset;
    nread = size - ku.uio_resid;
    if (copyout(kbuf, buf_ptr, nread)) {
        *err = EFAULT;
        return -1;
    }
    lock_release(of->lock);
    kfree(kbuf);
    return nread;
}



/*
 * file system calls for open/close
 */
int sys_open(userptr_t path, int openflags, mode_t mode, int *errp) {
    int fd, i, result;
    struct vnode *v;
    struct openfile *of = NULL;

    if (path == NULL || (userptr_t)path == (userptr_t)0) {
        *errp = EFAULT;
        return -1;
    }

    char *kbuffer = (char *)kmalloc(PATH_MAX * sizeof(char));
    if (kbuffer == NULL) {
        *errp = ENOMEM;
        return -1;
    }

    size_t len;
    result = copyinstr((const_userptr_t)path, kbuffer, PATH_MAX, &len);
    if (result) {
        kfree(kbuffer);
        *errp = result;
        return -1;
    }

    if ((vaddr_t)path >= 0x80000000) {
        kfree(kbuffer);
        *errp = EFAULT;
        return -1;
    }

    result = vfs_open(kbuffer, openflags, mode, &v);
    kfree(kbuffer);
    if (result) {
        *errp = result;
        return -1;
    }

    for (i = 0; i < SYSTEM_OPEN_MAX; i++) {
        if (systemFileTable[i].vn == NULL) {
            of = &systemFileTable[i];
            of->vn = v;
            of->offset = 0;
            break;
        }
    }

    if (of == NULL) {
        vfs_close(v);
        return ENFILE;
    }

    for (fd = 3; fd < OPEN_MAX; fd++) {
        if (curproc->fileTable[fd] == NULL) {
            curproc->fileTable[fd] = of;
            break;
        }
    }

    if (fd == OPEN_MAX) {
        vfs_close(v);
        return EMFILE;
    }

    if (openflags & O_APPEND) {
        struct stat filestat;
        result = VOP_STAT(v, &filestat);
        if (result) {
            curproc->fileTable[fd] = NULL;
            vfs_close(v);
            *errp = result;
            return -1;
        }
        of->offset = filestat.st_size;
    } else {
        of->offset = 0;
    }

    switch (openflags & O_ACCMODE) {
        case O_RDONLY:
            of->mode_open = O_RDONLY;
            break;
        case O_WRONLY:
            of->mode_open = O_WRONLY;
            break;
        case O_RDWR:
            of->mode_open = O_RDWR;
            break;
        default:
            curproc->fileTable[fd] = NULL;
            vfs_close(v);
            return EINVAL;
    }

    of->lock = lock_create("file_lock");
    if (of->lock == NULL) {
        curproc->fileTable[fd] = NULL;
        vfs_close(v);
        *errp = ENOMEM;
        return -1;
    }

    of->countRef = 1;

    *errp = 0;
    return fd;
}

int sys_close(int fd) {
    struct openfile *of;

    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    of = curproc->fileTable[fd];
    if (of == NULL) {
        return EBADF;
    }

    lock_acquire(of->lock);
    of->countRef--;

    curproc->fileTable[fd] = NULL;

    if (of->countRef == 0) {
        vfs_close(of->vn);

        lock_release(of->lock);
        lock_destroy(of->lock);
    } else {
        lock_release(of->lock);
    }

    return 0;
}

int sys_chdir(const char *user_path) {
    struct vnode *dir_vnode;
    char *kernel_buffer;
    int result;

    KASSERT(curthread != NULL);
    KASSERT(curthread->t_proc != NULL);

    kernel_buffer = (char *)kmalloc(PATH_MAX);
    if (kernel_buffer == NULL) {
        return ENOMEM;
    }

    result = copyinstr((const_userptr_t)user_path, kernel_buffer, PATH_MAX, NULL);
    if (result) {
        kfree(kernel_buffer);
        return result;
    }

    result = vfs_open(kernel_buffer, O_RDONLY, 0, &dir_vnode);
    kfree(kernel_buffer);
    if (result) {
        return result;
    }

    result = vfs_setcurdir(dir_vnode);
    if (result) {
        vfs_close(dir_vnode);
        return result;
    }

    vfs_close(dir_vnode);
    return 0;
}

int sys_lseek(int fd, off_t pos, int whence, int32_t *retval_low32, int32_t *retval_upp32) {
    KASSERT(curproc != NULL);

    if (fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
        return EBADF;
    }

    struct openfile *of = curproc->fileTable[fd];
    if (of == NULL) {
        return EBADF;
    }

    if (!VOP_ISSEEKABLE(of->vn)) {
        return ESPIPE;
    }

    struct stat file_stat;
    int err;
    off_t new_offset;

    lock_acquire(of->lock);

    switch (whence) {
        case SEEK_SET:
            if (pos < 0) {
                lock_release(of->lock);
                return EINVAL;
            }
            new_offset = pos;
            break;

        case SEEK_CUR:
            if (pos < 0 && -pos > of->offset) {
                lock_release(of->lock);
                return EINVAL;
            }
            new_offset = of->offset + pos;
            break;

        case SEEK_END:
            err = VOP_STAT(of->vn, &file_stat);
            if (err) {
                lock_release(of->lock);
                return err;
            }
            if (pos < 0 && -pos > file_stat.st_size) {
                lock_release(of->lock);
                return EINVAL;
            }
            new_offset = file_stat.st_size + pos;
            break;

        default:
            lock_release(of->lock);
            return EINVAL;
    }

    of->offset = new_offset;
    lock_release(of->lock);

    *retval_low32 = (int32_t)(new_offset & 0xFFFFFFFF);
    *retval_upp32 = (int32_t)((new_offset >> 32) & 0xFFFFFFFF);

    return 0;
}

int sys_getcwd(char *buf, size_t buflen, int32_t *retval) {
    KASSERT(curthread != NULL);
    KASSERT(curthread->t_proc != NULL);

    if (buf == NULL) {
        return EFAULT;
    }
    if (buflen == 0) {
        return EINVAL;
    }

    struct uio u;
    struct iovec iov;

    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len = buflen;

    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = buflen;
    u.uio_offset = 0;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = curthread->t_proc->p_addrspace;

    int err = vfs_getcwd(&u);
    if (err) {
        return err;
    }

    if (u.uio_resid > 0) {
        return ERANGE;
    }

    *retval = buflen - u.uio_resid;
    return 0;
}
