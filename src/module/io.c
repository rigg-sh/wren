#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "scheduler.h"
#include "stat.h"
#include "vm.h"
#include "wren.h"

#include <stdio.h>
#include <fcntl.h>

typedef struct sFileRequestData
{
  WrenHandle* fiber;
  uv_buf_t buffer;
} FileRequestData;

static const int stdinDescriptor = 0;

// Handle to the Stat class object.
static WrenHandle* statClass = NULL;

// Handle to the Stdin class object.
static WrenHandle* stdinClass = NULL;

// Handle to an onData_() method call. Called when libuv provides data on stdin.
static WrenHandle* stdinOnData = NULL;

// The stream used to read from stdin. Initialized on the first read.
static uv_stream_t* stdinStream = NULL;

// True if stdin has been set to raw mode.
static bool isStdinRaw = false;

// Frees all resources related to stdin.
static void shutdownStdin()
{
  if (stdinStream != NULL)
  {
    uv_tty_reset_mode();
    uv_close((uv_handle_t*)stdinStream, NULL);
    free(stdinStream);
    stdinStream = NULL;
  }
  
  if (stdinClass != NULL)
  {
    wrenReleaseHandle(getVM(), stdinClass);
    stdinClass = NULL;
  }
  
  if (stdinOnData != NULL)
  {
    wrenReleaseHandle(getVM(), stdinOnData);
    stdinOnData = NULL;
  }
}

void ioShutdown()
{
  shutdownStdin();
  
  if (statClass != NULL)
  {
    wrenReleaseHandle(getVM(), statClass);
    statClass = NULL;
  }
}

// If [request] failed with an error, sends the runtime error to the VM and
// frees the request.
//
// Returns true if an error was reported.
static bool handleRequestError(uv_fs_t* request)
{
  if (request->result >= 0) return false;

  FileRequestData* data = (FileRequestData*)request->data;
  WrenHandle* fiber = (WrenHandle*)data->fiber;
  
  int error = (int)request->result;
  free(data);
  uv_fs_req_cleanup(request);
  free(request);
  
  schedulerResumeError(fiber, uv_strerror(error));
  return true;
}

// Allocates a new request that resumes [fiber] when it completes.
uv_fs_t* createRequest(WrenHandle* fiber)
{
  uv_fs_t* request = (uv_fs_t*)malloc(sizeof(uv_fs_t));
  
  FileRequestData* data = (FileRequestData*)malloc(sizeof(FileRequestData));
  data->fiber = fiber;
  
  request->data = data;
  return request;
}

// Releases resources used by [request].
//
// Returns the fiber that should be resumed after [request] completes.
WrenHandle* freeRequest(uv_fs_t* request)
{
  FileRequestData* data = (FileRequestData*)request->data;
  WrenHandle* fiber = data->fiber;
  
  free(data);
  uv_fs_req_cleanup(request);
  free(request);
  
  return fiber;
}

static void directoryListCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  uv_dirent_t entry;

  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotCount(fiber, 3);
  wrenSetSlotNewList(fiber, 2);
  
  while (uv_fs_scandir_next(request, &entry) != UV_EOF)
  {
    wrenSetSlotString(fiber, 1, entry.name);
    wrenInsertInList(fiber, 2, -1, 1);
  }
  
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void directoryList(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 2));
  
  // TODO: Check return.
  uv_fs_scandir(getLoop(), request, path, 0, directoryListCallback);
}

void fileAllocate(WrenFiber* fiber)
{
  // Store the file descriptor in the foreign data, so that we can get to it
  // in the finalizer.
  int* fd = (int*)wrenSetSlotNewForeign(fiber, 0, 0, sizeof(int));
  *fd = (int)wrenGetSlotDouble(fiber, 1);
}

void fileFinalize(void* data)
{
  int fd = *(int*)data;
  
  // Already closed.
  if (fd == -1) return;
  
  uv_fs_t request;
  uv_fs_close(getLoop(), &request, fd, NULL);
  uv_fs_req_cleanup(&request);
}

static void fileDeleteCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  schedulerResume(freeRequest(request), false);
}

void fileDelete(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 2));
  
  // TODO: Check return.
  uv_fs_unlink(getLoop(), request, path, fileDeleteCallback);
}

static void fileOpenCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  double fd = (double)request->result;
  schedulerResume(freeRequest(request), true);
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotDouble(fiber, 2, fd);
  schedulerFinishResume();
}

// The UNIX file flags have specified names but not values. So we define our
// own values in FileFlags and remap them to the host OS's values here.
static int mapFileFlags(int flags)
{
  int result = 0;
  
  // Note: These must be kept in sync with FileFlags in io.wren.
  if (flags & 0x01) result |= O_RDONLY;
  if (flags & 0x02) result |= O_WRONLY;
  if (flags & 0x04) result |= O_RDWR;
  if (flags & 0x08) result |= O_SYNC;
  if (flags & 0x10) result |= O_CREAT;
  if (flags & 0x20) result |= O_TRUNC;
  if (flags & 0x40) result |= O_EXCL;
  
  return result;
}

void fileOpen(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  int flags = (int)wrenGetSlotDouble(fiber, 2);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 3));

  // TODO: Allow controlling access.
  uv_fs_open(getLoop(), request, path, mapFileFlags(flags), S_IRUSR | S_IWUSR,
             fileOpenCallback);
}

// Called by libuv when the stat call for size completes.
static void fileSizeCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  double size = (double)request->statbuf.st_size;
  schedulerResume(freeRequest(request), true);
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotDouble(fiber, 2, size);
  schedulerFinishResume();
}

void fileSizePath(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 2));
  uv_fs_stat(getLoop(), request, path, fileSizeCallback);
}

static void fileCloseCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  schedulerResume(freeRequest(request), false);
}

void fileClose(WrenFiber* fiber)
{
  int* foreign = (int*)wrenGetSlotForeign(fiber, 0);
  int fd = *foreign;

  // If it's already closed, we're done.
  if (fd == -1)
  {
    wrenSetSlotBool(fiber, 0, true);
    return;
  }

  // Mark it closed immediately.
  *foreign = -1;

  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 1));
  uv_fs_close(getLoop(), request, fd, fileCloseCallback);
  wrenSetSlotBool(fiber, 0, false);
}

void fileDescriptor(WrenFiber* fiber)
{
  int* foreign = (int*)wrenGetSlotForeign(fiber, 0);
  int fd = *foreign;
  wrenSetSlotDouble(fiber, 0, fd);
}

static void fileReadBytesCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  FileRequestData* data = (FileRequestData*)request->data;
  uv_buf_t buffer = data->buffer;
  size_t count = request->result;

  // TODO: Having to copy the bytes here is a drag. It would be good if Wren's
  // embedding API supported a way to *give* it bytes that were previously
  // allocated using Wren's own allocator.
  schedulerResume(freeRequest(request), true);
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotBytes(fiber, 2, buffer.base, count);
  schedulerFinishResume();

  // TODO: Likewise, freeing this after we resume is lame.
  free(buffer.base);
}

void fileReadBytes(WrenFiber* fiber)
{
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 3));

  int fd = *(int*)wrenGetSlotForeign(fiber, 0);
  // TODO: Assert fd != -1.

  FileRequestData* data = (FileRequestData*)request->data;
  size_t length = (size_t)wrenGetSlotDouble(fiber, 1);
  size_t offset = (size_t)wrenGetSlotDouble(fiber, 2);

  data->buffer.len = length;
  data->buffer.base = (char*)malloc(length);

  uv_fs_read(getLoop(), request, fd, &data->buffer, 1, offset,
             fileReadBytesCallback);
}

static void realPathCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotCount(fiber, 3);
  wrenSetSlotString(fiber, 2, (char*)request->ptr);
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void fileRealPath(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 2));
  uv_fs_realpath(getLoop(), request, path, realPathCallback);
}

// Called by libuv when the stat call completes.
static void statCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotCount(fiber, 3);
  
  // Get a handle to the Stat class. We'll hang on to this so we don't have to
  // look it up by name every time.
  if (statClass == NULL)
  {
    wrenGetVariable(vm, "io", "Stat", 0);
    statClass = wrenGetSlotHandle(fiber, 0);
  }
  
  // Create a foreign Stat object to store the stat struct.
  wrenSetSlotHandle(fiber, 2, statClass);
  wrenSetSlotNewForeign(fiber, 2, 2, sizeof(uv_stat_t));
  
  // Copy the stat data.
  uv_stat_t* data = (uv_stat_t*)wrenGetSlotForeign(fiber, 2);
  *data = request->statbuf;
  
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void fileStat(WrenFiber* fiber)
{
  int fd = *(int*)wrenGetSlotForeign(fiber, 0);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 1));
  uv_fs_fstat(getLoop(), request, fd, statCallback);
}

void fileSize(WrenFiber* fiber)
{
  int fd = *(int*)wrenGetSlotForeign(fiber, 0);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 1));
  uv_fs_fstat(getLoop(), request, fd, fileSizeCallback);
}

static void fileWriteBytesCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
 
  FileRequestData* data = (FileRequestData*)request->data;
  free(data->buffer.base);

  schedulerResume(freeRequest(request), false);
}

void fileWriteBytes(WrenFiber* fiber)
{
  int fd = *(int*)wrenGetSlotForeign(fiber, 0);
  int length;
  const char* bytes = wrenGetSlotBytes(fiber, 1, &length);
  size_t offset = (size_t)wrenGetSlotDouble(fiber, 2);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 3));
  
  FileRequestData* data = (FileRequestData*)request->data;

  data->buffer.len = length;
  // TODO: Instead of copying, just create a WrenHandle for the byte string and
  // hold on to it in the request until the write is done.
  // TODO: Handle allocation failure.
  data->buffer.base = (char*)malloc(length);
  memcpy(data->buffer.base, bytes, length);

  uv_fs_write(getLoop(), request, fd, &data->buffer, 1, offset,
              fileWriteBytesCallback);
}

void statPath(WrenFiber* fiber)
{
  const char* path = wrenGetSlotString(fiber, 1);
  uv_fs_t* request = createRequest(wrenGetSlotHandle(fiber, 2));
  uv_fs_stat(getLoop(), request, path, statCallback);
}

void statBlockCount(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_blocks);
}

void statBlockSize(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_blksize);
}

void statDevice(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_dev);
}

void statGroup(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_gid);
}

void statInode(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_ino);
}

void statLinkCount(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_nlink);
}

void statMode(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_mode);
}

void statSize(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_size);
}

void statSpecialDevice(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_rdev);
}

void statUser(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotDouble(fiber, 0, (double)stat->st_uid);
}

void statIsDirectory(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotBool(fiber, 0, S_ISDIR(stat->st_mode));
}

void statIsFile(WrenFiber* fiber)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(fiber, 0);
  wrenSetSlotBool(fiber, 0, S_ISREG(stat->st_mode));
}

// Sets up the stdin stream if not already initialized.
static void initStdin()
{
  if (stdinStream == NULL)
  {
    if (uv_guess_handle(stdinDescriptor) == UV_TTY)
    {
      // stdin is connected to a terminal.
      uv_tty_t* handle = (uv_tty_t*)malloc(sizeof(uv_tty_t));
      uv_tty_init(getLoop(), handle, stdinDescriptor, true);
      
      stdinStream = (uv_stream_t*)handle;
    }
    else
    {
      // stdin is a pipe or a file.
      uv_pipe_t* handle = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
      uv_pipe_init(getLoop(), handle, false);
      uv_pipe_open(handle, stdinDescriptor);
      stdinStream = (uv_stream_t*)handle;
    }
  }
}

void stdinIsRaw(WrenFiber* fiber)
{
  wrenSetSlotBool(fiber, 0, isStdinRaw);
}

void stdinIsRawSet(WrenFiber* fiber)
{
  initStdin();
  
  isStdinRaw = wrenGetSlotBool(fiber, 1);
  
  if (uv_guess_handle(stdinDescriptor) == UV_TTY)
  {
    uv_tty_t* handle = (uv_tty_t*)stdinStream;
    uv_tty_set_mode(handle, isStdinRaw ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
  }
  else
  {
    // Can't set raw mode when not talking to a TTY.
    // TODO: Make this a runtime error?
  }
}

void stdinIsTerminal(WrenFiber* fiber)
{
  initStdin();
  wrenSetSlotBool(fiber, 0, uv_guess_handle(stdinDescriptor) == UV_TTY);
}

void stdoutFlush(WrenFiber* fiber)
{
  fflush(stdout);
  wrenSetSlotNull(fiber, 0);
}

static void allocCallback(uv_handle_t* handle, size_t suggestedSize,
                          uv_buf_t* buf)
{
  // TODO: Handle allocation failure.
  buf->base = (char*)malloc(suggestedSize);
  buf->len = suggestedSize;
}

static void stdinReadCallback(uv_stream_t* stream, ssize_t numRead,
                              const uv_buf_t* buffer)
{
  WrenVM* vm = getVM();
  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  
  if (stdinClass == NULL)
  {
    wrenSetSlotCount(fiber, 1);
    wrenGetVariable(vm, "io", "Stdin", 0);
    stdinClass = wrenGetSlotHandle(fiber, 0);
  }
  
  if (stdinOnData == NULL)
  {
    stdinOnData = wrenMakeCallHandle(vm, "onData_(_)");
  }
  
  // If stdin was closed, send null to let io.wren know.
  if (numRead == UV_EOF)
  {
    wrenSetSlotCount(fiber, 2);
    wrenSetSlotHandle(fiber, 0, stdinClass);
    wrenSetSlotNull(fiber, 1);
    wrenCall(fiber, stdinOnData);
    
    shutdownStdin();
    return;
  }

  // TODO: Handle other errors.

  // TODO: Having to copy the bytes here is a drag. It would be good if Wren's
  // embedding API supported a way to *give* it bytes that were previously
  // allocated using Wren's own allocator.
  wrenSetSlotCount(fiber, 2);
  wrenSetSlotHandle(fiber, 0, stdinClass);
  wrenSetSlotBytes(fiber, 1, buffer->base, numRead);
  wrenCall(fiber, stdinOnData);

  // TODO: Likewise, freeing this after we resume is lame.
  free(buffer->base);
}

void stdinReadStart(WrenFiber* fiber)
{
  initStdin();
  uv_read_start(stdinStream, allocCallback, stdinReadCallback);
  // TODO: Check return.
}

void stdinReadStop(WrenFiber* fiber)
{
  uv_read_stop(stdinStream);
}
