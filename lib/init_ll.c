/*
 * initializes fuse fs with low level fuse apis
 */

#define FUSE_USE_VERSION 30

#include <stdio.h>
#include <stdlib.h>
#include <fuse.h>
#include <fuse_lowlevel.h>

// for ENOENT
#include <errno.h>

#include <sys/stat.h>

// inode tables to map inodes to paths
#include "../include/inode_table.h"

// utils used for path funcs
#include "../include/utils.h"

// in case cannot find inode for an entry or calculating inode not necessary
#define FUSE_UNKNOWN_INO 0xffffffff

struct fu_ll_ctx {
  struct fu_table_t *table;
  struct fuse_operations *ops;
};
void fu_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  printf("\n\n Inside fu_ll_lookup:\n");
  // reply entry for lookup
  struct fuse_entry_param e = {0};
  e.generation = 0;
  e.entry_timeout = 0;
  e.attr_timeout = 0;

  struct fu_ll_ctx *ctx = fuse_req_userdata(req);

  struct fu_buf_t path_buf = fu_get_path(ctx->table, parent, name);

  char *path = path_buf.data;


  int res = ctx->ops->getattr(path, &e.attr);

  if (res != 0) {

    printf("looking up and error, name: %s, path: %s\n", name, path);
    fuse_reply_err(req, -res);
    goto free_buf;
  }

  struct fu_node_t *node = fu_table_lookup(ctx->table, parent, name);
  if (!node) {
    printf("looking up and error, name: %s, path: %s\n", name, path);
    fuse_reply_err(req, ENOENT);
    goto free_buf;
  }
  e.ino = fu_node_inode(node);

  printf("looking up and success!! name: %s,path: %s\n", name, path);
  fuse_reply_entry(req, &e);

free_buf:
  printf("\n");
  fu_buf_free(&path_buf);
}

void fu_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  printf("\n\nInside fu_ll_getattr:\n");
  struct fu_ll_ctx *ctx = fuse_req_userdata(req);
  struct stat stbuf = {0};

  struct fu_buf_t path_buf = fu_get_path(ctx->table, ino, NULL);

  char *path = path_buf.data;

  int res = ctx->ops->getattr(path, &stbuf);
  if (res == 0) {
    printf("getting attr and success!! : %s with ino %ld \n", path, ino);
    fuse_reply_attr(req, &stbuf, 1.0);
  }
  else {
    printf("getting attr and error : %s with ino %ld \n", path, ino);
    fuse_reply_err(req, -res);
  }

  printf("\n");
  fu_buf_free(&path_buf);
}

// custom directory handle
struct fu_dh_t {
  struct fu_buf_t contents;
  struct fu_buf_t path_buf;
  int filled;
  fuse_ino_t inode;
  // fuse request for the file handle
  fuse_req_t req;
  // high level operations file handle
  uint64_t dh;
};

void fu_dh_free(struct fu_dh_t *dh) {
  fu_buf_free(&dh->contents);
  fu_buf_free(&dh->path_buf);
  free(dh);
}
void fu_ll_opendir(fuse_req_t req, fuse_ino_t inode,
     struct fuse_file_info *llfi) {
  printf("\n\nInside opendir:\n");
  struct fu_ll_ctx *ctx = fuse_req_userdata(req);

  struct fu_dh_t *dh_ll = malloc(sizeof(struct fu_dh_t));
  fu_buf_init(&dh_ll->contents, 256);

  dh_ll->filled = 0;
  dh_ll->inode = inode;

  dh_ll->path_buf  = fu_get_path(ctx->table, inode, NULL);
  const char *path = dh_ll->path_buf.data;

  int res = ctx->ops->opendir(path, llfi);

  if (res != 0) {
    printf("Cannot open path: %s\n", dh_ll->path_buf.data);
    fuse_reply_err(req, -res);
    fu_dh_free(dh_ll);
    return;
  }

  // overwrite the dir handle with our low level one
  dh_ll->dh = llfi->fh;
  llfi->fh = (uint64_t) dh_ll;

  printf("Opened dir successfully! with path: %s\n", dh_ll->path_buf.data);
  // TODO: handle interrupted sycall in case fuse_reply_open returns error
  // release dir in that case must be called
  fuse_reply_open(req, llfi);
}

int fill_dir(void *buf, const char *dir_name, const struct stat *st, off_t off) {
  struct fu_dh_t *dh_ll = buf;
  struct fu_ll_ctx  *ctx = fuse_req_userdata(dh_ll->req);
  int newlen = 0;
  struct stat new_st = *st;

  printf("Trying to add dir with name (%s) at off (%ld)\n", dir_name, off);

  struct fu_node_t *node = fu_table_get(ctx->table, st->st_ino);
  // replace the high level inode with our own one,
  if (node) {
    new_st.st_ino = fu_node_inode(node);
    printf("got the dir in hash table, giving it our inode (%ld) \n", new_st.st_ino);
  }
  else {
    printf("could not find the dir in the hash table, adding it under pid (%ld)\n", dh_ll->inode);
    if (fu_table_add(ctx->table, dh_ll->inode, dir_name, new_st.st_ino)) {
      printf("added dir successfully!\n");
    }
    else {
      printf("could not add dir, maybe a pid error, resetting the inode to unknown\n");
      new_st.st_ino = FUSE_UNKNOWN_INO;
    }
  }

  if (off) {
    printf("got an offset, so only adding it in that length\n");
    // not filled, just add it at the right offset
    dh_ll->filled = 0;

    newlen = dh_ll->contents.size + fuse_add_direntry(
        dh_ll->req,
        dh_ll->contents.data + dh_ll->contents.size,
        dh_ll->contents.cap - dh_ll->contents.size,
        dir_name,
        &new_st, off);

    if (newlen > dh_ll->contents.cap) {
      printf("out buffer size was not enough, stopping further entries\n\n");
      // buffer not enough, stop adding new entries
      return 1;
    }
  }
  else {
    printf("no offset so appending entries :) \n");
    // no offset, so have to append, check if enough cap to add another entry
    newlen = dh_ll->contents.size + fuse_add_direntry(dh_ll->req, NULL, 0, dir_name, NULL, 0);
    if (newlen > dh_ll->contents.cap) {
      fu_buf_resize(&dh_ll->contents, newlen);
    }

    fuse_add_direntry(
        dh_ll->req,
        dh_ll->contents.data + dh_ll->contents.size,
        dh_ll->contents.cap - dh_ll->contents.size,
        dir_name,
        &new_st, newlen);
  }

  printf("updating the size  of dir buffer to %d\n\n", newlen);
  // update buffer size
  dh_ll->contents.size = newlen;

  return 0;
}

void fu_ll_readdir(fuse_req_t req, fuse_ino_t inode, size_t size,
     off_t off, struct fuse_file_info *llfi) {
  printf("\n\nInside read dir\n");
  struct fu_ll_ctx *ctx = fuse_req_userdata(req);
  struct fu_dh_t *dh_ll = (struct fu_dh_t *) llfi->fh;

  if (!off) {
    dh_ll->filled = 0;
  }

  printf("dir path: %s\n", dh_ll->path_buf.data);

  if (!dh_ll->filled) {
    // try to fill the dh_ll, reset buffer to get rid of old dirs
    fu_buf_reset(&dh_ll->contents);
    // make sure that it has enough size to accomodate dirs
    if (size > dh_ll->contents.size) {
      fu_buf_resize(&dh_ll->contents, size);
    }


    // temporarily change dir handle to the high level one
    llfi->fh = dh_ll->dh;

    dh_ll->req = req;
    printf("reading data into dh with resetted buffers!\n");
    int res = ctx->ops->readdir(
        dh_ll->path_buf.data, dh_ll, fill_dir, off, llfi);

    llfi->fh = (uint64_t) dh_ll;

    if (res != 0) {
      // failed to fill the buffer
      dh_ll->filled = 0;
      fuse_reply_err(req, -res);
    }
    else {
      printf("filled the data successfully into buffers!\n");
      // filled successfully!
      dh_ll->filled = 1;
    }
  }

  // should be filled by now if no errors
  if (dh_ll->filled) {
    if (off < dh_ll->contents.size) {
      printf("dir is filled and got correct offset (%ld) against total size (%d), replying data back :)\n", off, dh_ll->contents.size);
      // valid offset, normalize size now
      size = off + size > dh_ll->contents.size ?
        dh_ll->contents.size - off : size;
      fuse_reply_buf(req, dh_ll->contents.data + off, size);
    }
    else {
      printf("offset (%ld) against size (%d) too far off, returning null buffer :(\n", off, dh_ll->contents.size);
      // offset too far off, return null buffer
      fuse_reply_buf(req, NULL, 0);
    }
  }
  else {
    printf("buffer not filled, just giving back the data that is there\n");
    // buffer not filled, just return what ever we have, ignore offset
    fuse_reply_buf(req, dh_ll->contents.data, dh_ll->contents.size);
  }
}

void fu_ll_releasedir(fuse_req_t req, fuse_ino_t inode,
     struct fuse_file_info *llfi) {
  printf("\n\nInside releasedir\n");
  struct fu_ll_ctx *ctx = fuse_req_userdata(req);
  struct fu_dh_t *dh_ll = (struct fu_dh_t *) llfi->fh;

  llfi->fh = dh_ll->dh;
  int res = ctx->ops->releasedir(dh_ll->path_buf.data, llfi);

  printf("released dir with path %s successfully!\n", dh_ll->path_buf.data);

  fu_dh_free(dh_ll);

  fuse_reply_err(req, -res);
}

// TODO: implement all the low level ops
struct fuse_lowlevel_ops llops = {
  .lookup = fu_ll_lookup,
  .getattr = fu_ll_getattr,
  .opendir = fu_ll_opendir,
  .readdir = fu_ll_readdir,
  .releasedir = fu_ll_releasedir,
  .open = NULL,
  .read = NULL
};

int init(int argc, char *argv[], struct fuse_operations *ops) {
  // TODO: get fuse args out altogether, implement custom args parser
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

  int res, multithreaded, foreground;
  char *mountpoint;

  // TODO: replace with a custom implementation
  res = fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground);
  if (res == -1) {
    printf("error parsing cmdline\n");
    return -1;
  }

  struct fuse_chan *ch = fuse_mount(mountpoint, &args);
  if (!ch) {
    printf("error mounting the fs and creating its channel\n");
    goto err_free;
  }

  // TODO: use foreground
  fuse_daemonize(1);

  // setup internal tables to track inodes
  struct fu_table_t *table = fu_table_alloc();
  fu_table_add(table, 0, "/", FUSE_ROOT_ID);

  struct fu_ll_ctx ctx = {
    .table = table,
    .ops = ops
  };

  struct fuse_session *se =
    fuse_lowlevel_new(&args, &llops, sizeof(llops), &ctx);

  if (!se) {
    printf("error creating a new low level fuse session\n");
    goto err_unmount;
  }

  res = fuse_set_signal_handlers(se);
  if (res == -1) {
    printf("error setting signal handdlers on the fuse session\n");
    goto err_session;
  }

  fuse_session_add_chan(se, ch);

  // TODO: add support for multithreaded session loop (using fuse_session_loop_mt)
  res = fuse_session_loop(se);

  fuse_session_remove_chan(ch);
  fuse_remove_signal_handlers(se);

err_session:
  fuse_session_destroy(se);
err_unmount:
  fuse_unmount(mountpoint, ch);
err_free:
  // need to free args as fuse may internally allocate args to modify them
  fuse_opt_free_args(&args);

  free(mountpoint);

  return res ? 1 : 0;
}
